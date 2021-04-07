#include "asynchttprequest.h"

#include "sdkconfig.h"
#define LOG_LOCAL_LEVEL CONFIG_LOG_LOCAL_LEVEL_ASYNC_HTTP

// system includes
#include <chrono>
#include <cstring>
#include <assert.h>

// esp-idf includes
#include <esp_log.h>

// local includes
#include "cleanuphelper.h"
#include "taskutils.h"
#include "tickchrono.h"

using namespace std::chrono_literals;

namespace {
constexpr const char * const TAG = "ASYNC_HTTP";

constexpr int TASK_RUNNING = BIT0;
constexpr int START_REQUEST_BIT = BIT1;
constexpr int REQUEST_FINISHED_BIT = BIT2;
} // namespace

AsyncHttpRequest::AsyncHttpRequest(espcpputils::CoreAffinity coreAffinity) :
    m_coreAffinity{coreAffinity}
{
    assert(eventGroup.handle);

    eventGroup.clearBits(TASK_RUNNING);
    eventGroup.clearBits(START_REQUEST_BIT);
    eventGroup.clearBits(REQUEST_FINISHED_BIT);

    if (const auto failed = startTask())
    {
        ESP_LOGE(TAG, "%s", failed->c_str());
    }
}

AsyncHttpRequest::~AsyncHttpRequest()
{
    // TODO exit task
    abort();
}

std::optional<std::string> AsyncHttpRequest::start(const std::string &url)
{
    if (!taskHandle)
    {
        if (const auto failed = startTask())
            return *failed;
    }

    buf.clear();
    this->url = url;

    config = esp_http_client_config_t{};
    config.url = url.c_str();
    config.event_handler = httpEventHandler;
    config.user_data = this;

    request.construct(&config);

    auto helper = cpputils::makeCleanupHelper([&request=request](){ request.destruct(); });

    if (!request->handle)
        return "request could not be constructed";

    eventGroup.clearBits(REQUEST_FINISHED_BIT);
    eventGroup.setBits(START_REQUEST_BIT);
    helper.disarm(); // this stops the helper from destructing the request

    return std::nullopt;
}

bool AsyncHttpRequest::finished() const
{
    return eventGroup.getBits() & REQUEST_FINISHED_BIT;
}

std::optional<std::string> AsyncHttpRequest::failed() const
{
    if (result != ESP_OK)
        return std::string{"http request failed: "} + esp_err_to_name(result);

    if (request->get_status_code() != 200)
        return std::string{"http request failed: "} + std::to_string(request->get_status_code());

    return std::nullopt;
}

void AsyncHttpRequest::cleanup()
{
    request.destruct();
    buf.clear();
}

esp_err_t AsyncHttpRequest::httpEventHandler(esp_http_client_event_t *evt)
{
    auto _this = (AsyncHttpRequest*)evt->user_data;

    assert(_this);

    switch(evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (evt->header_key && evt->header_value)
        {
            if (strcasecmp(evt->header_key, "Content-Length") == 0)
            {
                unsigned int size;
                if (std::sscanf(evt->header_value, "%u", &size) == 1)
                {
                    //ESP_LOGD(TAG, "reserving %u bytes for http buffer", size);
                    _this->buf.reserve(size);
                }
                else
                {
                    ESP_LOGW(TAG, "Could not parse Content-Length header \"%s\"", evt->header_value);
                }
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        //assert(!_this->request->is_chunked_response());

        if (evt->data && evt->data_len)
            _this->buf += std::string((const char *)evt->data, evt->data_len);

        break;
    default:;
    }

    return ESP_OK;
}

void AsyncHttpRequest::requestTask(void *ptr)
{
    auto helper = cpputils::makeCleanupHelper([](){ vTaskDelete(NULL); });

    auto _this = (AsyncHttpRequest*)ptr;

    assert(_this);
    _this->eventGroup.setBits(TASK_RUNNING);
    auto helper2 = cpputils::makeCleanupHelper([=](){ _this->eventGroup.clearBits(TASK_RUNNING); });

    while (true)
    {
        {
            const auto bits = _this->eventGroup.waitBits(START_REQUEST_BIT, true, true, espcpputils::ticks{1s}.count());
            if (!(bits & START_REQUEST_BIT))
                continue;
        }

        assert(_this->request.constructed());
        assert(!(_this->eventGroup.getBits() & REQUEST_FINISHED_BIT));

        do
        {
            _this->result = _this->request->perform();
        }
        while (_this->result == EAGAIN || _this->result == EINPROGRESS);

        _this->eventGroup.setBits(REQUEST_FINISHED_BIT);
    }
}

std::optional<std::string> AsyncHttpRequest::startTask()
{
    if (taskHandle)
        return "task already started";

    const auto result = espcpputils::createTask(requestTask, "httpRequestTask", 4096, this, 10, &taskHandle, m_coreAffinity);
    if (result != pdPASS)
        return std::string{"failed creating http task "} + std::to_string(result);

    if (!taskHandle)
        return "http task handle is null";

    ESP_LOGD(TAG, "created http task");
    return std::nullopt;
}
