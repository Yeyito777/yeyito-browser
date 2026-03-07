// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "net/network_request_buffer.h"

#include <QtCore/QJsonDocument>

namespace QtWebEngineCore {

QJsonObject NetworkRequestEntry::toSummaryJson() const
{
    QJsonObject obj;
    obj[QLatin1String("id")] = static_cast<qint64>(requestId);
    obj[QLatin1String("url")] = url;
    obj[QLatin1String("method")] = method;
    obj[QLatin1String("status")] = httpStatusCode;
    obj[QLatin1String("type")] = resourceType;
    obj[QLatin1String("mimeType")] = mimeType;
    obj[QLatin1String("size")] = static_cast<qint64>(rawBodyBytes);
    obj[QLatin1String("cached")] = wasCached;
    if (netError != 0)
        obj[QLatin1String("netError")] = netError;
    return obj;
}

QJsonObject NetworkRequestEntry::toDetailJson() const
{
    QJsonObject obj;
    obj[QLatin1String("id")] = static_cast<qint64>(requestId);
    obj[QLatin1String("url")] = url;
    obj[QLatin1String("originalUrl")] = originalUrl;
    obj[QLatin1String("method")] = method;
    obj[QLatin1String("status")] = httpStatusCode;
    obj[QLatin1String("type")] = resourceType;
    obj[QLatin1String("mimeType")] = mimeType;
    obj[QLatin1String("cached")] = wasCached;
    obj[QLatin1String("netError")] = netError;
    obj[QLatin1String("rawBodyBytes")] = static_cast<qint64>(rawBodyBytes);
    obj[QLatin1String("totalReceivedBytes")] = static_cast<qint64>(totalReceivedBytes);

    if (!remoteEndpoint.isEmpty())
        obj[QLatin1String("remoteEndpoint")] = remoteEndpoint;

    if (!requestHeaders.isEmpty())
        obj[QLatin1String("requestHeaders")] = requestHeaders;

    QJsonObject timing;
    timing[QLatin1String("dnsStartMs")] = dnsStartMs;
    timing[QLatin1String("dnsEndMs")] = dnsEndMs;
    timing[QLatin1String("connectStartMs")] = connectStartMs;
    timing[QLatin1String("connectEndMs")] = connectEndMs;
    timing[QLatin1String("sslStartMs")] = sslStartMs;
    timing[QLatin1String("sslEndMs")] = sslEndMs;
    timing[QLatin1String("sendStartMs")] = sendStartMs;
    timing[QLatin1String("sendEndMs")] = sendEndMs;
    timing[QLatin1String("receiveHeadersStartMs")] = receiveHeadersStartMs;
    timing[QLatin1String("receiveHeadersEndMs")] = receiveHeadersEndMs;
    obj[QLatin1String("timing")] = timing;

    return obj;
}

void NetworkRequestBuffer::addEntry(NetworkRequestEntry entry)
{
    if (static_cast<int>(m_entries.size()) >= kMaxEntries)
        m_entries.erase(m_entries.begin());
    m_entries.push_back(std::move(entry));
}

void NetworkRequestBuffer::clear()
{
    m_entries.clear();
}

QString NetworkRequestBuffer::queryList() const
{
    QJsonArray arr;
    for (const auto &entry : m_entries)
        arr.append(entry.toSummaryJson());

    QJsonObject result;
    result[QLatin1String("requests")] = arr;
    result[QLatin1String("count")] = static_cast<int>(m_entries.size());
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
}

QString NetworkRequestBuffer::queryDetail(int64_t requestId) const
{
    for (const auto &entry : m_entries) {
        if (entry.requestId == requestId)
            return QString::fromUtf8(QJsonDocument(entry.toDetailJson()).toJson(QJsonDocument::Indented));
    }

    QJsonObject err;
    err[QLatin1String("error")] = QLatin1String("request not found");
    err[QLatin1String("requestId")] = static_cast<qint64>(requestId);
    return QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Indented));
}

} // namespace QtWebEngineCore
