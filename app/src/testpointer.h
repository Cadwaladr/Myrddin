// For tests only (--test-eval): a pointer that can be moved over the window and clicked where there
// is no real one ("pointer.move(900, 210)", "pointer.click(900, 210)"; window coordinates).
#pragma once

#include <QCoreApplication>
#include <QMouseEvent>
#include <QObject>
#include <QQuickWindow>

class TestPointer : public QObject {
    Q_OBJECT
public:
    explicit TestPointer(QQuickWindow* window, QObject* parent = nullptr) : QObject(parent), m_window(window) {}
    Q_INVOKABLE void move(qreal x, qreal y) { send(QEvent::MouseMove, x, y, Qt::NoButton, Qt::NoButton); }
    Q_INVOKABLE void click(qreal x, qreal y) {
        move(x, y);
        send(QEvent::MouseButtonPress, x, y, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, x, y, Qt::LeftButton, Qt::NoButton);
    }

private:
    void send(QEvent::Type type, qreal x, qreal y, Qt::MouseButton button, Qt::MouseButtons buttons) {
        if (!m_window) return;
        const QPointF at(x, y);
        QMouseEvent event(type, at, m_window->mapToGlobal(at), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window, &event);
    }
    QQuickWindow* m_window;
};
