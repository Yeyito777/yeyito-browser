// Copyright (C) 2026 Yeyito. All rights reserved.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "smooth_scroll_controller.h"
#include "render_widget_host_view_qt.h"

#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"

#include <QGuiApplication>
#include <QScreen>
#include <cmath>

namespace QtWebEngineCore {

SmoothScrollController::SmoothScrollController(RenderWidgetHostViewQt *rwhv, QObject *parent)
    : QObject(parent)
    , m_rwhv(rwhv)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    int interval = 16;
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        qreal hz = screen->refreshRate();
        if (hz > 0)
            interval = qMax(1, static_cast<int>(std::round(1000.0 / hz)));
    }
    m_timer.setInterval(interval);
    connect(&m_timer, &QTimer::timeout, this, &SmoothScrollController::tick);
}

SmoothScrollController::~SmoothScrollController()
{
    if (m_scrolling) {
        m_timer.stop();
        m_scrolling = false;
    }
}

void SmoothScrollController::scrollBy(int dx, int dy, double factor, int posX, int posY)
{
    m_dx += dx;
    m_dy += dy;
    m_factor = factor;

    if (!m_scrolling) {
        m_posX = posX;
        m_posY = posY;
        m_subPixelX = 0.0;
        m_subPixelY = 0.0;
        sendGestureScrollBegin(static_cast<float>(-dx), static_cast<float>(-dy));
        m_scrolling = true;
        m_elapsed.start();
        m_timer.start();
    }
}

void SmoothScrollController::stop()
{
    if (m_scrolling) {
        m_timer.stop();
        sendGestureScrollEnd();
        m_scrolling = false;
        m_dx = 0.0;
        m_dy = 0.0;
        m_subPixelX = 0.0;
        m_subPixelY = 0.0;
    }
}

void SmoothScrollController::tick()
{
    double dt = qMax(1.0, static_cast<double>(m_elapsed.restart()));
    // Time-based decay: same visual speed regardless of refresh rate.
    // At the reference 16ms tick, effectiveFactor == m_factor exactly.
    double effectiveFactor = 1.0 - std::pow(1.0 - m_factor, dt / 16.0);

    // Compute fractional step and accumulate subpixel remainder.
    // Pixels are emitted only when the accumulator crosses a whole-pixel
    // boundary, so the emission rate naturally decelerates to zero —
    // no min-1px clamping that would create a constant-velocity tail.
    double fracStepX = m_dx * effectiveFactor;
    double fracStepY = m_dy * effectiveFactor;

    m_subPixelX += fracStepX;
    m_subPixelY += fracStepY;

    // Truncate toward zero to extract whole pixels (symmetric for ±)
    int stepX = static_cast<int>(m_subPixelX);
    int stepY = static_cast<int>(m_subPixelY);

    m_subPixelX -= stepX;
    m_subPixelY -= stepY;

    m_dx -= fracStepX;
    m_dy -= fracStepY;

    if (stepX != 0 || stepY != 0)
        sendGestureScrollUpdate(stepX, stepY);

    // Stop when remaining delta is negligible
    if (std::abs(m_dx) < 0.01 && std::abs(m_dy) < 0.01) {
        m_timer.stop();
        sendGestureScrollEnd();
        m_scrolling = false;
        m_dx = 0.0;
        m_dy = 0.0;
        m_subPixelX = 0.0;
        m_subPixelY = 0.0;
    }
}

gfx::PointF SmoothScrollController::hitTestPosition()
{
    if (m_posX >= 0 && m_posY >= 0)
        return gfx::PointF(static_cast<float>(m_posX), static_cast<float>(m_posY));
    gfx::Rect bounds = m_rwhv->GetViewBounds();
    return gfx::PointF(bounds.width() / 2.0f, bounds.height() / 2.0f);
}

void SmoothScrollController::sendGestureScrollBegin(float deltaXHint, float deltaYHint)
{
    auto *host = m_rwhv->host();
    if (!host)
        return;

    gfx::PointF pos = hitTestPosition();

    blink::WebGestureEvent event(
        blink::WebInputEvent::Type::kGestureScrollBegin,
        blink::WebInputEvent::kNoModifiers,
        base::TimeTicks::Now());
    event.SetSourceDevice(blink::WebGestureDevice::kTouchpad);
    event.SetPositionInWidget(pos);
    // Provide direction hints so the compositor can latch to a scroll
    // node that can actually consume delta in the intended direction.
    // Without these, CanConsumeDelta() treats zero hints as "any node
    // matches" and may latch to a horizontally-scrollable child that
    // can't scroll vertically (e.g. a code block with overflow:auto).
    event.data.scroll_begin.delta_x_hint = deltaXHint;
    event.data.scroll_begin.delta_y_hint = deltaYHint;
    // When no specific element position was given (posX/posY == -1),
    // target the viewport directly to avoid hit-testing into a child
    // scrollable element that happens to be under the viewport center.
    event.data.scroll_begin.target_viewport = (m_posX < 0 && m_posY < 0);

    host->ForwardGestureEvent(event);
}

void SmoothScrollController::sendGestureScrollUpdate(int stepX, int stepY)
{
    auto *host = m_rwhv->host();
    if (!host)
        return;

    gfx::PointF pos = hitTestPosition();

    blink::WebGestureEvent event(
        blink::WebInputEvent::Type::kGestureScrollUpdate,
        blink::WebInputEvent::kNoModifiers,
        base::TimeTicks::Now());
    event.SetSourceDevice(blink::WebGestureDevice::kTouchpad);
    event.SetPositionInWidget(pos);
    event.data.scroll_update.delta_x = static_cast<float>(-stepX);
    event.data.scroll_update.delta_y = static_cast<float>(-stepY);

    host->ForwardGestureEvent(event);
}

void SmoothScrollController::sendGestureScrollEnd()
{
    auto *host = m_rwhv->host();
    if (!host)
        return;

    gfx::PointF pos = hitTestPosition();

    blink::WebGestureEvent event(
        blink::WebInputEvent::Type::kGestureScrollEnd,
        blink::WebInputEvent::kNoModifiers,
        base::TimeTicks::Now());
    event.SetSourceDevice(blink::WebGestureDevice::kTouchpad);
    event.SetPositionInWidget(pos);

    host->ForwardGestureEvent(event);
}

} // namespace QtWebEngineCore
