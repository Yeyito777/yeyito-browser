// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef NETWORK_REQUEST_BUFFER_H
#define NETWORK_REQUEST_BUFFER_H

#include <QtCore/QString>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include <cstdint>
#include <vector>

namespace QtWebEngineCore {

struct NetworkRequestEntry {
    int64_t requestId = 0;
    QString url;
    QString originalUrl;
    QString method;
    QString resourceType;
    QString mimeType;
    int httpStatusCode = 0;
    int netError = 0;
    bool wasCached = false;
    int64_t rawBodyBytes = 0;
    int64_t totalReceivedBytes = 0;

    // Timing in milliseconds relative to request_start (0 = not available)
    double dnsStartMs = 0;
    double dnsEndMs = 0;
    double connectStartMs = 0;
    double connectEndMs = 0;
    double sslStartMs = 0;
    double sslEndMs = 0;
    double sendStartMs = 0;
    double sendEndMs = 0;
    double receiveHeadersStartMs = 0;
    double receiveHeadersEndMs = 0;

    // Remote endpoint
    QString remoteEndpoint;

    // Request headers (name → value)
    QJsonObject requestHeaders;

    QJsonObject toSummaryJson() const;
    QJsonObject toDetailJson() const;
};

class NetworkRequestBuffer {
public:
    static constexpr int kMaxEntries = 1000;

    void addEntry(NetworkRequestEntry entry);
    void clear();

    QString queryList() const;
    QString queryDetail(int64_t requestId) const;

    int size() const { return static_cast<int>(m_entries.size()); }

private:
    std::vector<NetworkRequestEntry> m_entries;
};

} // namespace QtWebEngineCore

#endif // NETWORK_REQUEST_BUFFER_H
