#include "asynchttprequest.h"

#include "sdkconfig.h"
#define LOG_LOCAL_LEVEL CONFIG_LOG_LOCAL_LEVEL_ASYNC_HTTP

// system includes
#include <chrono>
#include <string_view>
#include <cstring>
#include <assert.h>
#include <algorithm>

// esp-idf includes
#include <esp_log.h>

// 3rdparty lib includes
#include <fmt/core.h>
#include <cpputils.h>

// local includes
#include "cleanuphelper.h"
#include "taskutils.h"
#include "tickchrono.h"

using namespace std::chrono_literals;

namespace {
constexpr const char * const TAG = "ASYNC_HTTP";

constexpr int TASK_RUNNING_BIT = BIT0;
constexpr int START_REQUEST_BIT = BIT1;
constexpr int REQUEST_RUNNING_BIT = BIT2;
constexpr int REQUEST_FINISHED_BIT = BIT3;
constexpr int END_TASK_BIT = BIT4;
constexpr int TASK_ENDED_BIT = BIT5;
constexpr int ABORT_REQUEST_BIT = BIT6;
} // namespace

AsyncHttpRequest::AsyncHttpRequest(const char *taskName, espcpputils::CoreAffinity coreAffinity) :
    m_taskName{taskName},
    m_coreAffinity{coreAffinity}
{
    assert(m_eventGroup.handle);
}

AsyncHttpRequest::~AsyncHttpRequest()
{
    endTask();
}

std::expected<void, std::string> AsyncHttpRequest::startTask()
{
    if (m_taskHandle)
    {
        constexpr auto msg = "http task handle is not null";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    if (const auto bits = m_eventGroup.getBits(); bits & TASK_RUNNING_BIT)
    {
        constexpr auto msg = "http task already running";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    m_eventGroup.clearBits(TASK_RUNNING_BIT | START_REQUEST_BIT | REQUEST_RUNNING_BIT | REQUEST_FINISHED_BIT | END_TASK_BIT | TASK_ENDED_BIT | ABORT_REQUEST_BIT);

    if (auto result = espcpputils::createTask(requestTask, m_taskName, 3096, this, 10, &m_taskHandle, m_coreAffinity);
        result != pdPASS)
    {
        auto msg = fmt::format("failed creating http task {}", result);
        ESP_LOGE(TAG, "%.*s", msg.size(), msg.data());
        return std::unexpected(std::move(msg));
    }

    if (!m_taskHandle)
    {
        constexpr auto msg = "http task handle is null";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    ESP_LOGD(TAG, "created http task %s", m_taskName);

    if (const auto bits = m_eventGroup.waitBits(TASK_RUNNING_BIT, false, false, std::chrono::ceil<espcpputils::ticks>(1s).count());
        bits & TASK_RUNNING_BIT)
        return {};

    ESP_LOGW(TAG, "http task %s TASK_RUNNING_BIT bit not yet set...", m_taskName);

    while (true)
        if (const auto bits = m_eventGroup.waitBits(TASK_RUNNING_BIT, false, false, portMAX_DELAY);
            bits & TASK_RUNNING_BIT)
            break;

    return {};
}

std::expected<void, std::string> AsyncHttpRequest::endTask()
{
    if (const auto bits = m_eventGroup.getBits();
        !(bits & TASK_RUNNING_BIT))
        return {};
    else if (bits & END_TASK_BIT)
    {
        constexpr auto msg = "Another end request is already pending";
        ESP_LOGE(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    m_eventGroup.setBits(END_TASK_BIT);

    if (const auto bits = m_eventGroup.waitBits(TASK_ENDED_BIT, true, false, std::chrono::ceil<espcpputils::ticks>(1s).count());
        bits & TASK_ENDED_BIT)
    {
        ESP_LOGD(TAG, "http task %s ended", m_taskName);
        return {};
    }

    ESP_LOGW(TAG, "http task %s TASK_ENDED_BIT bit not yet set...", m_taskName);

    while (true)
        if (const auto bits = m_eventGroup.waitBits(TASK_ENDED_BIT, true, false, portMAX_DELAY);
            bits & TASK_ENDED_BIT)
            break;

    ESP_LOGD(TAG, "http task %s ended", m_taskName);

    return {};
}

bool AsyncHttpRequest::taskRunning() const
{
    if (const auto bits = m_eventGroup.getBits();
        bits & TASK_RUNNING_BIT)
        return true;

    return false;
}

std::expected<void, std::string> AsyncHttpRequest::createClient(std::string_view url, esp_http_client_method_t method,
                                                               int timeout_ms)
{
    if (m_client)
    {
        constexpr auto msg = "m_client already created";
        ESP_LOGE(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    esp_http_client_config_t config{};
    config.url = url.data();
    config.max_authorization_retries = 1;
    config.method = method;
    config.timeout_ms = timeout_ms;
    config.event_handler = staticHttpEventHandler;
    config.user_data = this;
    config.is_async = true;

    m_client = espcpputils::http_client{&config};

    if (!m_client)
    {
        auto msg = fmt::format("http client could not be constructed (url={})", url);
        ESP_LOGE(TAG, "%.*s", msg.size(), msg.data());
        return std::unexpected(std::move(msg));
    }

    ESP_LOGD(TAG, "created http client %s", m_taskName);

    return {};
}

std::expected<void, std::string> AsyncHttpRequest::deleteClient()
{
    if (!m_client)
        return {};

    if (inProgress())
    {
        constexpr auto msg = "request still in progress";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    m_client = {};

    return {};
}

bool AsyncHttpRequest::hasClient() const
{
    return m_client;
}

std::expected<void, std::string> AsyncHttpRequest::start(std::string_view url,
                                                        esp_http_client_method_t method,
                                                        const std::map<std::string, std::string> &requestHeaders,
                                                        std::string_view requestBody, int timeout_ms)
{
    if (!m_taskHandle)
    {
        if (auto result = startTask(); !result)
            return std::unexpected(std::move(result).error());
    }

    if (inProgress())
    {
        constexpr auto msg = "another request still in progress";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    if (m_client)
    {
        ESP_LOGW(TAG, "old http client still constructed, destructing now");
        m_client = {};
    }

    if (auto result = createClient(url, method, timeout_ms); !result)
        return std::unexpected(std::move(result).error());

    for (auto iter = std::cbegin(requestHeaders); iter != std::cend(requestHeaders); iter++)
        if (const auto result = m_client.set_header(iter->first, iter->second); result != ESP_OK)
        {
            auto msg = fmt::format("m_client.set_header() failed: {} ({} {})", esp_err_to_name(result), iter->first, iter->second);
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }

    if (!requestBody.empty())
    {
        if (const auto result = m_client.open(requestBody.size()); result != ESP_OK)
        {
            auto msg = fmt::format("m_client.open() failed: {} ({})", esp_err_to_name(result), requestBody.size());
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }
        if (const auto written = m_client.write(requestBody); written < 0)
        {
            auto msg = fmt::format("m_client.write() failed: {}", written);
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }
        else if (written != requestBody.size())
        {
            auto msg = fmt::format("m_client.write() written size mismatch: {} != {}", written, requestBody.size());
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }
    }

    m_buf.clear();

    clearFinished();
    m_eventGroup.setBits(START_REQUEST_BIT);

    return {};
}

std::expected<void, std::string> AsyncHttpRequest::retry(std::optional<std::string_view> url,
                                                        std::optional<esp_http_client_method_t> method,
                                                        const std::map<std::string, std::string> &requestHeaders,
                                                        std::string_view requestBody
#ifndef OLD_IDF
                                                        , std::optional<int> timeout_ms
#endif
                                                        )
{
    if (!m_taskHandle)
    {
        if (auto result = startTask(); !result)
            return std::unexpected(std::move(result).error());
    }

    if (inProgress())
    {
        constexpr auto msg = "another request still in progress";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    if (!m_client)
    {
        constexpr auto msg = "http client is null";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    if (url)
        if (const auto result = m_client.set_url(*url); result != ESP_OK)
        {
            auto msg = fmt::format("m_client.set_url() failed: {} ({})", esp_err_to_name(result), *url);
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }

    if (method)
        if (const auto result = m_client.set_method(*method); result != ESP_OK)
        {
            auto msg = fmt::format("m_client.set_method() failed: {}", esp_err_to_name(result));
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }

#ifndef OLD_IDF
    if (timeout_ms)
        if (const auto result = m_client.set_timeout_ms(*timeout_ms); result != ESP_OK)
        {
            auto msg = fmt::format("m_client.set_timeout_ms() failed: {}", esp_err_to_name(result));
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }
#endif

    for (auto iter = std::cbegin(requestHeaders); iter != std::cend(requestHeaders); iter++)
        if (const auto result = m_client.set_header(iter->first, iter->second); result != ESP_OK)
        {
            auto msg = fmt::format("m_client.set_header() failed: {} ({} {})", esp_err_to_name(result), iter->first, iter->second);
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }

    if (!requestBody.empty())
    {
        if (const auto result = m_client.open(requestBody.size()); result != ESP_OK)
        {
            auto msg = fmt::format("m_client.open() failed: {} ({})", esp_err_to_name(result), requestBody.size());
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }
        if (const auto written = m_client.write(requestBody); written < 0)
        {
            auto msg = fmt::format("m_client.write() failed: {}", written);
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }
        else if (written != requestBody.size())
        {
            auto msg = fmt::format("m_client.write() written size mismatch: {} != {}", written, requestBody.size());
            ESP_LOGW(TAG, "%.*s", msg.size(), msg.data());
            return std::unexpected(std::move(msg));
        }
    }

    m_buf.clear();

    clearFinished();
    m_eventGroup.setBits(START_REQUEST_BIT);

    return {};
}

std::expected<void, std::string> AsyncHttpRequest::abort()
{
    if (const auto bits = m_eventGroup.getBits(); !(bits & (START_REQUEST_BIT | REQUEST_RUNNING_BIT)))
        return std::unexpected("no ota job is running!");
    else if (bits & ABORT_REQUEST_BIT)
        return std::unexpected("an abort has already been requested!");

    m_eventGroup.setBits(ABORT_REQUEST_BIT);
    ESP_LOGI(TAG, "http request abort requested");

    return {};
}

bool AsyncHttpRequest::inProgress() const
{
    return m_eventGroup.getBits() & (START_REQUEST_BIT | REQUEST_RUNNING_BIT);
}

bool AsyncHttpRequest::finished() const
{
    return m_eventGroup.getBits() & REQUEST_FINISHED_BIT;
}

std::expected<void, std::string> AsyncHttpRequest::result() const
{
    if (const auto bits = m_eventGroup.getBits();
        bits & REQUEST_RUNNING_BIT)
    {
        constexpr auto msg = "request still running";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }
    else if (!(bits & REQUEST_FINISHED_BIT))
    {
        constexpr auto msg = "request not finished";
        ESP_LOGW(TAG, "%s", msg);
        return std::unexpected(msg);
    }

    if (m_result != ESP_OK)
        return std::unexpected(fmt::format("http request failed: {}", esp_err_to_name(m_result)));

    return {};
}

void AsyncHttpRequest::clearFinished()
{
    m_eventGroup.clearBits(REQUEST_FINISHED_BIT);
}

esp_err_t AsyncHttpRequest::httpEventHandler(esp_http_client_event_t *evt)
{
    switch(evt->event_id)
    {
    case HTTP_EVENT_ON_HEADER:
        if (evt->header_key && evt->header_value)
        {
            if (m_collectResponseHeaders)
                m_responseHeaders.emplace(std::make_pair(evt->header_key, evt->header_value));
            if (strcasecmp(evt->header_key, "Content-Length") == 0)
            {
                unsigned int size;
                if (std::sscanf(evt->header_value, "%u", &size) == 1)
                {
                    //ESP_LOGD(TAG, "reserving %u bytes for http buffer", size);
                    m_buf.reserve(std::min(size, m_sizeLimit));
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
        else if (m_buf.size() >= m_sizeLimit)
            return ESP_ERR_NO_MEM;
        else
        {
            const auto remainingSize = m_sizeLimit - m_buf.size();
            m_buf += std::string_view((const char *)evt->data, std::min<size_t>(evt->data_len, remainingSize));
            if (remainingSize < evt->data_len)
                return ESP_ERR_NO_MEM;
        }

        break;
    default:
        ;
    }

    return ESP_OK;
}

esp_err_t AsyncHttpRequest::staticHttpEventHandler(esp_http_client_event_t *evt)
{
    auto _this = reinterpret_cast<AsyncHttpRequest*>(evt->user_data);

    assert(_this);

    return _this->httpEventHandler(evt);
}

void AsyncHttpRequest::requestTask(void *ptr)
{
    auto _this = reinterpret_cast<AsyncHttpRequest*>(ptr);

    assert(_this);

    _this->requestTask();
}

void AsyncHttpRequest::requestTask()
{
    m_eventGroup.setBits(TASK_RUNNING_BIT);

    // cleanup on task exit
    auto helper = cpputils::makeCleanupHelper([&](){
        m_eventGroup.clearBits(TASK_RUNNING_BIT);
        m_eventGroup.setBits(TASK_ENDED_BIT);
        m_taskHandle = NULL;
        vTaskDelete(NULL);
    });

    while (true)
    {
        if (const auto bits = m_eventGroup.waitBits(START_REQUEST_BIT|END_TASK_BIT, true, false, portMAX_DELAY);
            bits & END_TASK_BIT)
            break;
        else if (!(bits & START_REQUEST_BIT))
            continue;

        assert(m_client);

        {
            const auto bits = m_eventGroup.getBits();
            assert(!(bits & START_REQUEST_BIT));
            assert(!(bits & REQUEST_RUNNING_BIT));
            assert(!(bits & REQUEST_FINISHED_BIT));
        }

        m_eventGroup.setBits(REQUEST_RUNNING_BIT);

        auto helper2 = cpputils::makeCleanupHelper([&](){
            m_eventGroup.clearBits(REQUEST_RUNNING_BIT | ABORT_REQUEST_BIT);
            m_eventGroup.setBits(REQUEST_FINISHED_BIT);
        });

        {
            esp_err_t result;
            do
            {
                m_buf.clear();
                result = m_client.perform();
                ESP_LOG_LEVEL_LOCAL((cpputils::is_in(result, ESP_OK, EAGAIN, EINPROGRESS) ? ESP_LOG_DEBUG : ESP_LOG_WARN),
                                    TAG, "m_client.perform() returned: %s", result == EAGAIN ? "EAGAIN" : esp_err_to_name(result));

                if (m_eventGroup.clearBits(ABORT_REQUEST_BIT) & ABORT_REQUEST_BIT)
                {
                    ESP_LOGW(TAG, "abort request received");
                    result = ESP_FAIL;
                    break;
                }
            }
            while (result == EAGAIN || result == EINPROGRESS);

            m_result = result;
            m_statusCode = m_client.get_status_code();
        }

        {
            const auto result = m_client.close();
            ESP_LOGD(TAG, "m_client.close() returned: %s", esp_err_to_name(result));
        }
    }
}
