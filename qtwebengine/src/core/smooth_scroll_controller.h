// Copyright (C) 2026 Yeyito. All rights reserved.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef SMOOTH_SCROLL_CONTROLLER_H
#define SMOOTH_SCROLL_CONTROLLER_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "ui/gfx/geometry/point_f.h"

namespace QtWebEngineCore {

class RenderWidgetHostViewQt;

class SmoothScrollController : public QObject
{
    Q_OBJECT
public:
    explicit SmoothScrollController(RenderWidgetHostViewQt *rwhv, QObject *parent = nullptr);
    ~SmoothScrollController();

    void scrollBy(int dx, int dy, double factor, int posX = -1, int posY = -1);
    void stop();

private Q_SLOTS:
    void tick();

private:
    gfx::PointF hitTestPosition();
    void sendGestureScrollBegin(float deltaXHint, float deltaYHint);
    void sendGestureScrollUpdate(int stepX, int stepY);
    void sendGestureScrollEnd();

    RenderWidgetHostViewQt *m_rwhv;
    QTimer m_timer;
    QElapsedTimer m_elapsed;
    double m_dx = 0.0;
    double m_dy = 0.0;
    double m_subPixelX = 0.0;
    double m_subPixelY = 0.0;
    double m_factor = 0.3;
    int m_posX = -1;
    int m_posY = -1;
    bool m_scrolling = false;
};

} // namespace QtWebEngineCore

#endif // SMOOTH_SCROLL_CONTROLLER_H
