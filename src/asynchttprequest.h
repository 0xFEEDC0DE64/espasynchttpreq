#pragma once

// system includes
#include <string>
#include <string_view>
#include <map>

// esp-idf includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>

// 3rdparty lib includes
#include <tl/expected.hpp>

// local includes
#include "wrappers/http_client.h"
#include "wrappers/event_group.h"
#include "taskutils.h"

class AsyncHttpRequest
{
public:
    AsyncHttpRequest(const char *taskName="httpRequestTask", espcpputils::CoreAffinity coreAffinity=espcpputils::CoreAffinity::Core1);
    ~AsyncHttpRequest();

    tl::expected<void, std::string> startTask();
    tl::expected<void, std::string> endTask();

    tl::expected<void, std::string> createClient(std::string_view url);
    tl::expected<void, std::string> deleteClient();

    tl::expected<void, std::string> start(std::string_view url,
                                          esp_http_client_method_t method = HTTP_METHOD_GET,
                                          const std::map<std::string, std::string> &requestHeaders = {},
                                          std::string_view requestBody = {});
    tl::expected<void, std::string> abort();

    bool inProgress() const;

    bool finished() const;
    tl::expected<void, std::string> result() const;

    int statusCode() const { return m_statusCode; }

    void clearFinished();

    const std::string &buffer() const { return m_buf; }
    std::string &&takeBuffer() { return std::move(m_buf); }

    std::size_t sizeLimit() const { return m_sizeLimit; }
    void setSizeLimit(std::size_t sizeLimit) { m_sizeLimit = sizeLimit; }

    bool collectResponseHeaders() const { return m_collectResponseHeaders; }
    void setCollectResponseHeaders(bool collectResponseHeaders) { m_collectResponseHeaders = collectResponseHeaders; }

    const std::map<std::string, std::string> &responseHeaders() const { return m_responseHeaders; }
    std::map<std::string, std::string> &&takeResponseHeaders() { return std::move(m_responseHeaders); }

private:
    esp_err_t httpEventHandler(esp_http_client_event_t *evt);
    static esp_err_t staticHttpEventHandler(esp_http_client_event_t *evt);
    static void requestTask(void *ptr);
    void requestTask();

    espcpputils::http_client m_client;
    std::string m_buf;
    TaskHandle_t m_taskHandle{NULL};
    espcpputils::event_group m_eventGroup;
    esp_err_t m_result{};
    int m_statusCode{};
    std::size_t m_sizeLimit{4096};
    bool m_collectResponseHeaders{};
    std::map<std::string, std::string> m_responseHeaders;

    const char * const m_taskName;
    const espcpputils::CoreAffinity m_coreAffinity;
};
