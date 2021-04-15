#pragma once

// system includes
#include <optional>
#include <string>

// esp-idf includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>

// local includes
#include "wrappers/http_client.h"
#include "wrappers/event_group.h"
#include "taskutils.h"

class AsyncHttpRequest
{
public:
    AsyncHttpRequest(const char *taskName="httpRequestTask", espcpputils::CoreAffinity coreAffinity=espcpputils::CoreAffinity::Core1);
    ~AsyncHttpRequest();

    std::optional<std::string> startTask();
    std::optional<std::string> endTask();

    std::optional<std::string> createClient(const std::string &url);
    std::optional<std::string> deleteClient();

    std::optional<std::string> start(const std::string &url);

    bool inProgress() const;

    bool finished() const;
    std::optional<std::string> failed() const;

    void clearFinished();

    const std::string &buffer() const { return buf; }
    std::string &&takeBuffer() { return std::move(buf); }

private:
    static esp_err_t httpEventHandler(esp_http_client_event_t *evt);
    static void requestTask(void *ptr);

    espcpputils::http_client client;
    std::string buf;
    TaskHandle_t taskHandle{NULL};
    espcpputils::event_group eventGroup;
    esp_err_t result{};
    int statusCode{};

    const char * const m_taskName;
    const espcpputils::CoreAffinity m_coreAffinity;
};
