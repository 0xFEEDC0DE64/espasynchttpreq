#include "asynchttprequest.h"

#include "sdkconfig.h"
#define LOG_LOCAL_LEVEL CONFIG_LOG_LOCAL_LEVEL_ASYNC_HTTP

// system includes
#include <chrono>
#include <string_view>
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
constexpr int REQUEST_RUNNING_BIT = BIT2;
constexpr int REQUEST_FINISHED_BIT = BIT3;
constexpr int END_TASK_BIT = BIT4;
constexpr int TASK_ENDED = BIT5;
} // namespace

AsyncHttpRequest::AsyncHttpRequest(const char *taskName, espcpputils::CoreAffinity coreAffinity) :
    m_taskName{taskName},
    m_coreAffinity{coreAffinity}
{
    assert(eventGroup.handle);
}

AsyncHttpRequest::~AsyncHttpRequest()
{
    endTask();
}

std::optional<std::string> AsyncHttpRequest::startTask()
{
    if (const auto bits = eventGroup.getBits();
        bits & TASK_RUNNING || taskHandle)
    {
        constexpr auto msg = "task already started";
        ESP_LOGW(TAG, "%s", msg);
        return msg;
    }

    eventGroup.clearBits(TASK_RUNNING | START_REQUEST_BIT | REQUEST_RUNNING_BIT | REQUEST_FINISHED_BIT | END_TASK_BIT | TASK_ENDED);

    const auto result = espcpputils::createTask(requestTask, m_taskName, 4096, this, 10, &taskHandle, m_coreAffinity);
    if (result != pdPASS)
    {
        auto msg = std::string{"failed creating http task "} + std::to_string(result);
        ESP_LOGE(TAG, "%s", msg.c_str());
        return msg;
    }

    if (!taskHandle)
    {
        constexpr auto msg = "http task handle is null";
        ESP_LOGW(TAG, "%s", msg);
        return msg;
    }

    ESP_LOGD(TAG, "created http task %s", m_taskName);

    if (const auto bits = eventGroup.waitBits(TASK_RUNNING, false, false, std::chrono::ceil<espcpputils::ticks>(1s).count());
        bits & TASK_RUNNING)
        return std::nullopt;

    ESP_LOGW(TAG, "http task %s TASK_RUNNING bit not yet set...", m_taskName);

    while (true)
        if (const auto bits = eventGroup.waitBits(TASK_RUNNING, false, false, portMAX_DELAY);
            bits & TASK_RUNNING)
            break;

    return std::nullopt;
}

std::optional<std::string> AsyncHttpRequest::endTask()
{
    if (const auto bits = eventGroup.getBits();
        !(bits & TASK_RUNNING))
        return std::nullopt;
    else if (bits & END_TASK_BIT)
    {
        constexpr auto msg = "Another end request is already pending";
        ESP_LOGE(TAG, "%s", msg);
        return msg;
    }

    eventGroup.setBits(END_TASK_BIT);

    if (const auto bits = eventGroup.waitBits(TASK_ENDED, true, false, std::chrono::ceil<espcpputils::ticks>(1s).count());
        bits & TASK_ENDED)
    {
        ESP_LOGD(TAG, "http task %s ended", m_taskName);
        return std::nullopt;
    }

    ESP_LOGW(TAG, "http task %s TASK_ENDED bit not yet set...", m_taskName);

    while (true)
        if (const auto bits = eventGroup.waitBits(TASK_ENDED, true, false, portMAX_DELAY);
            bits & TASK_ENDED)
            break;

    ESP_LOGD(TAG, "http task %s ended", m_taskName);

    return std::nullopt;
}

std::optional<std::string> AsyncHttpRequest::createClient(const std::string &url)
{
    if (client)
    {
        constexpr auto msg = "client already created";
        ESP_LOGE(TAG, "%s", msg);
        return msg;
    }

    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.event_handler = httpEventHandler;
    config.user_data = this;

    client = espcpputils::http_client{&config};

    if (!client)
    {
        constexpr auto msg = "http client could not be constructed";
        ESP_LOGE(TAG, "%s", msg);
        return msg;
    }

    ESP_LOGD(TAG, "created http client %s", m_taskName);

    return std::nullopt;
}

std::optional<std::string> AsyncHttpRequest::deleteClient()
{
    if (!client)
        return std::nullopt;

    if (inProgress())
    {
        constexpr auto msg = "another request still in progress";
        ESP_LOGW(TAG, "%s", msg);
        return msg;
    }

    client = {};

    return std::nullopt;
}

std::optional<std::string> AsyncHttpRequest::start(const std::string &url)
{
    if (!taskHandle)
    {
        if (const auto failed = startTask())
            return *failed;
    }

    if (inProgress())
    {
        constexpr auto msg = "another request still in progress";
        ESP_LOGW(TAG, "%s", msg);
        return msg;
    }

    if (!client)
    {
        if (const auto failed = createClient(url))
            return *failed;
    }
    else
    {
        const auto result = client.set_url(url.c_str());
        ESP_LOG_LEVEL_LOCAL((result == ESP_OK ? ESP_LOG_DEBUG : ESP_LOG_ERROR), TAG, "client.set_url() returned: %s (%s)", esp_err_to_name(result), url.c_str());
        if (result != ESP_OK)
            return std::string{"client.set_url() failed: "} + esp_err_to_name(result) + " (" + url + ')';
    }

    buf.clear();

    clearFinished();
    eventGroup.setBits(START_REQUEST_BIT);

    return std::nullopt;
}

bool AsyncHttpRequest::inProgress() const
{
    return eventGroup.getBits() & (START_REQUEST_BIT | REQUEST_RUNNING_BIT);
}

bool AsyncHttpRequest::finished() const
{
    return eventGroup.getBits() & REQUEST_FINISHED_BIT;
}

std::optional<std::string> AsyncHttpRequest::failed() const
{
    if (const auto bits = eventGroup.getBits();
        bits & REQUEST_RUNNING_BIT)
    {
        constexpr auto msg = "request still running";
        ESP_LOGW(TAG, "%s", msg);
        return msg;
    }
    else if (!(bits & REQUEST_FINISHED_BIT))
    {
        constexpr auto msg = "request not finished";
        ESP_LOGW(TAG, "%s", msg);
        return msg;
    }

    if (result != ESP_OK)
        return std::string{"http request failed: "} + esp_err_to_name(result);

    if (statusCode != HttpStatus_Ok)
        return std::string{"http request failed: "} + std::to_string(statusCode);

    return std::nullopt;
}

void AsyncHttpRequest::clearFinished()
{
    eventGroup.clearBits(REQUEST_FINISHED_BIT);
}

esp_err_t AsyncHttpRequest::httpEventHandler(esp_http_client_event_t *evt)
{
    auto _this = reinterpret_cast<AsyncHttpRequest*>(evt->user_data);

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
        if (!evt->data)
            ESP_LOGW(TAG, "handler with invalid data ptr");
        else if (evt->data_len <= 0)
            ESP_LOGW(TAG, "handler with invalid data_len %i", evt->data_len);
        else
            _this->buf += std::string_view((const char *)evt->data, evt->data_len);

        break;
    default:;
    }

    return ESP_OK;
}

void AsyncHttpRequest::requestTask(void *ptr)
{
    auto _this = reinterpret_cast<AsyncHttpRequest*>(ptr);

    assert(_this);

    _this->eventGroup.setBits(TASK_RUNNING);

    // cleanup on task exit
    auto helper = cpputils::makeCleanupHelper([&](){
        _this->eventGroup.clearBits(TASK_RUNNING);
        _this->eventGroup.setBits(TASK_ENDED);
        _this->taskHandle = NULL;
        vTaskDelete(NULL);
    });

    while (true)
    {
        if (const auto bits = _this->eventGroup.waitBits(START_REQUEST_BIT|END_TASK_BIT, true, false, portMAX_DELAY);
            bits & END_TASK_BIT)
            break;
        else if (!(bits & START_REQUEST_BIT))
            continue;

        assert(_this->client);

        {
            const auto bits = _this->eventGroup.getBits();
            assert(!(bits & START_REQUEST_BIT));
            assert(!(bits & REQUEST_RUNNING_BIT));
            assert(!(bits & REQUEST_FINISHED_BIT));
        }

        _this->eventGroup.setBits(REQUEST_RUNNING_BIT);

        auto helper2 = cpputils::makeCleanupHelper([&](){
            _this->eventGroup.clearBits(REQUEST_RUNNING_BIT);
            _this->eventGroup.setBits(REQUEST_FINISHED_BIT);
        });

        {
            esp_err_t result;
            do
                result = _this->client.perform();
            while (result == EAGAIN || result == EINPROGRESS);
            ESP_LOG_LEVEL_LOCAL((result == ESP_OK ? ESP_LOG_VERBOSE : ESP_LOG_ERROR), TAG, "client.perform() returned: %s", esp_err_to_name(result));
            _this->result = result;
            _this->statusCode = _this->client.get_status_code();
        }

        // workaround for esp-idf bug, every request after the first one fails with ESP_ERR_HTTP_FETCH_HEADER
        const auto result = _this->client.close();
        ESP_LOG_LEVEL_LOCAL((result == ESP_OK ? ESP_LOG_VERBOSE : ESP_LOG_ERROR), TAG, "client.close() returned: %s", esp_err_to_name(result));
        if (result != ESP_OK)
            _this->client = {};
    }
}
