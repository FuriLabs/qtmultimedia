/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2016 Research In Motion
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qdeclarativevideooutput_render_p.h"
#include "qdeclarativevideooutput_p.h"
#include <QtMultimedia/qabstractvideofilter.h>
#include <QtCore/qobject.h>
#include <QtCore/qloggingcategory.h>
#include <private/qmediapluginloader_p.h>
#include <private/qsgvideonode_p.h>

#if QT_CONFIG(opengl)
#include <QtGui/QOpenGLContext>
#endif

#include <QtQuick/QQuickWindow>
#include <QtCore/QRunnable>

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(qLcVideo)

Q_GLOBAL_STATIC_WITH_ARGS(QMediaPluginLoader, videoNodeFactoryLoader,
        (QSGVideoNodeFactoryInterface_iid, QLatin1String("video/videonode"), Qt::CaseInsensitive))

QDeclarativeVideoBackend::QDeclarativeVideoBackend(QDeclarativeVideoOutput *parent)
    : q(parent),
      m_frameChanged(false)
{
    m_surface = new QSGVideoItemSurface(this);
    QObject::connect(m_surface, SIGNAL(surfaceFormatChanged(QVideoSurfaceFormat)),
                     q, SLOT(_q_updateNativeSize()), Qt::QueuedConnection);

    // Prioritize the plugin requested by the environment
    QString requestedVideoNode = QString::fromLatin1(qgetenv("QT_VIDEONODE"));

    const auto keys = videoNodeFactoryLoader()->keys();
    for (const QString &key : keys) {
        QObject *instance = videoNodeFactoryLoader()->instance(key);
        QSGVideoNodeFactoryInterface* plugin = qobject_cast<QSGVideoNodeFactoryInterface*>(instance);
        if (plugin) {
            if (key == requestedVideoNode)
                m_videoNodeFactories.prepend(plugin);
            else
                m_videoNodeFactories.append(plugin);
            qCDebug(qLcVideo) << "found videonode plugin" << key << plugin;
        }
    }

    // Append existing node factories as fallback if we have no plugins
    m_videoNodeFactories.append(&m_i420Factory);
    m_videoNodeFactories.append(&m_rgbFactory);
    m_videoNodeFactories.append(&m_textureFactory);
}

QDeclarativeVideoBackend::~QDeclarativeVideoBackend()
{
    delete m_surface;
}

void QDeclarativeVideoBackend::appendFilter(QAbstractVideoFilter *filter)
{
    QMutexLocker lock(&m_frameMutex);
    m_filters.append(Filter(filter));
}

void QDeclarativeVideoBackend::clearFilters()
{
    QMutexLocker lock(&m_frameMutex);
    scheduleDeleteFilterResources();
    m_filters.clear();
}

class FilterRunnableDeleter : public QRunnable
{
public:
    FilterRunnableDeleter(const QList<QVideoFilterRunnable *> &runnables) : m_runnables(runnables) { }
    void run() override {
        for (QVideoFilterRunnable *runnable : qAsConst(m_runnables))
            delete runnable;
    }
private:
    QList<QVideoFilterRunnable *> m_runnables;
};

void QDeclarativeVideoBackend::scheduleDeleteFilterResources()
{
    if (!q->window())
        return;

    QList<QVideoFilterRunnable *> runnables;
    for (int i = 0; i < m_filters.count(); ++i) {
        if (m_filters[i].runnable) {
            runnables.append(m_filters[i].runnable);
            m_filters[i].runnable = nullptr;
        }
    }

    if (!runnables.isEmpty()) {
        // Request the scenegraph to run our cleanup job on the render thread.
        // The execution of our QRunnable may happen after the QML tree including the QAbstractVideoFilter instance is
        // destroyed on the main thread so no references to it must be used during cleanup.
        q->window()->scheduleRenderJob(new FilterRunnableDeleter(runnables), QQuickWindow::BeforeSynchronizingStage);
    }
}

void QDeclarativeVideoBackend::releaseResources()
{
    // Called on the gui thread when the window is closed or changed.
    invalidateSceneGraph();
}

void QDeclarativeVideoBackend::invalidateSceneGraph()
{
    // Called on the render thread, e.g. when the context is lost.
    QMutexLocker lock(&m_frameMutex);
    for (int i = 0; i < m_filters.count(); ++i) {
        if (m_filters[i].runnable) {
            delete m_filters[i].runnable;
            m_filters[i].runnable = nullptr;
        }
    }
}

void QDeclarativeVideoBackend::itemChange(QQuickItem::ItemChange change,
                                      const QQuickItem::ItemChangeData &changeData)
{
    if (change == QQuickItem::ItemSceneChange) {
        if (changeData.window)
            QObject::connect(changeData.window, SIGNAL(sceneGraphInvalidated()),
                             q, SLOT(_q_invalidateSceneGraph()), Qt::DirectConnection);
    }
}

QSize QDeclarativeVideoBackend::nativeSize() const
{
    return m_surfaceFormat.sizeHint();
}

void QDeclarativeVideoBackend::updateGeometry()
{
    const QRectF viewport = m_surfaceFormat.viewport();
    const QSizeF frameSize = m_surfaceFormat.frameSize();
    const QRectF normalizedViewport(viewport.x() / frameSize.width(),
                                    viewport.y() / frameSize.height(),
                                    viewport.width() / frameSize.width(),
                                    viewport.height() / frameSize.height());
    const QRectF rect(0, 0, q->width(), q->height());
    if (nativeSize().isEmpty()) {
        m_renderedRect = rect;
        m_sourceTextureRect = normalizedViewport;
    } else if (q->fillMode() == QDeclarativeVideoOutput::Stretch) {
        m_renderedRect = rect;
        m_sourceTextureRect = normalizedViewport;
    } else if (q->fillMode() == QDeclarativeVideoOutput::PreserveAspectFit) {
        m_sourceTextureRect = normalizedViewport;
        m_renderedRect = q->contentRect();
    } else if (q->fillMode() == QDeclarativeVideoOutput::PreserveAspectCrop) {
        m_renderedRect = rect;
        const qreal contentHeight = q->contentRect().height();
        const qreal contentWidth = q->contentRect().width();

        // Calculate the size of the source rectangle without taking the viewport into account
        const qreal relativeOffsetLeft = -q->contentRect().left() / contentWidth;
        const qreal relativeOffsetTop = -q->contentRect().top() / contentHeight;
        const qreal relativeWidth = rect.width() / contentWidth;
        const qreal relativeHeight = rect.height() / contentHeight;

        // Now take the viewport size into account
        const qreal totalOffsetLeft = normalizedViewport.x() + relativeOffsetLeft * normalizedViewport.width();
        const qreal totalOffsetTop = normalizedViewport.y() + relativeOffsetTop * normalizedViewport.height();
        const qreal totalWidth = normalizedViewport.width() * relativeWidth;
        const qreal totalHeight = normalizedViewport.height() * relativeHeight;

        if (qIsDefaultAspect(q->orientation())) {
            m_sourceTextureRect = QRectF(totalOffsetLeft, totalOffsetTop,
                                         totalWidth, totalHeight);
        } else {
            m_sourceTextureRect = QRectF(totalOffsetTop, totalOffsetLeft,
                                         totalHeight, totalWidth);
        }
    }

    if (m_surfaceFormat.scanLineDirection() == QVideoSurfaceFormat::BottomToTop) {
        qreal top = m_sourceTextureRect.top();
        m_sourceTextureRect.setTop(m_sourceTextureRect.bottom());
        m_sourceTextureRect.setBottom(top);
    }

    if (m_surfaceFormat.isMirrored()) {
        qreal left = m_sourceTextureRect.left();
        m_sourceTextureRect.setLeft(m_sourceTextureRect.right());
        m_sourceTextureRect.setRight(left);
    }
}

QSGNode *QDeclarativeVideoBackend::updatePaintNode(QSGNode *oldNode,
                                                           QQuickItem::UpdatePaintNodeData *data)
{
    Q_UNUSED(data);
    QSGVideoNode *videoNode = static_cast<QSGVideoNode *>(oldNode);

    QMutexLocker lock(&m_frameMutex);

#if QT_CONFIG(opengl)
    if (!m_glContext) {
        m_glContext = QOpenGLContext::currentContext();
        m_surface->scheduleOpenGLContextUpdate();

        // Internal mechanism to call back the surface renderer from the QtQuick render thread
        QObject *obj = m_surface->property("_q_GLThreadCallback").value<QObject*>();
        if (obj) {
            QEvent ev(QEvent::User);
            obj->event(&ev);
        }
    }
#endif

    bool isFrameModified = false;
    if (m_frameChanged) {
        // Run the VideoFilter if there is one. This must be done before potentially changing the videonode below.
        if (m_frame.isValid() && !m_filters.isEmpty()) {
            for (int i = 0; i < m_filters.count(); ++i) {
                QAbstractVideoFilter *filter = m_filters[i].filter;
                QVideoFilterRunnable *&runnable = m_filters[i].runnable;
                if (filter && filter->isActive()) {
                    // Create the filter runnable if not yet done. Ownership is taken and is tied to this thread, on which rendering happens.
                    if (!runnable)
                        runnable = filter->createFilterRunnable();
                    if (!runnable)
                        continue;

                    QVideoFilterRunnable::RunFlags flags;
                    if (i == m_filters.count() - 1)
                        flags |= QVideoFilterRunnable::LastInChain;

                    QVideoFrame newFrame = runnable->run(&m_frame, m_surfaceFormat, flags);

                    if (newFrame.isValid() && newFrame != m_frame) {
                        isFrameModified = true;
                        m_frame = newFrame;
                    }
                }
            }
        }

        if (videoNode && (videoNode->pixelFormat() != m_frame.pixelFormat() || videoNode->handleType() != m_frame.handleType())) {
            qCDebug(qLcVideo) << "updatePaintNode: deleting old video node because frame format changed";
            delete videoNode;
            videoNode = nullptr;
        }

        if (!m_frame.isValid()) {
            qCDebug(qLcVideo) << "updatePaintNode: no frames yet";
            m_frameChanged = false;
            return nullptr;
        }

        if (!videoNode) {
            for (QSGVideoNodeFactoryInterface* factory : qAsConst(m_videoNodeFactories)) {
                // Get a node that supports our frame. The surface is irrelevant, our
                // QSGVideoItemSurface supports (logically) anything.
                QVideoSurfaceFormat nodeFormat(m_frame.size(), m_frame.pixelFormat(), m_frame.handleType());
                nodeFormat.setYCbCrColorSpace(m_surfaceFormat.yCbCrColorSpace());
                nodeFormat.setScanLineDirection(m_surfaceFormat.scanLineDirection());
                nodeFormat.setViewport(m_surfaceFormat.viewport());
                nodeFormat.setFrameRate(m_surfaceFormat.frameRate());
                // Update current surface format if something has changed.
                m_surfaceFormat = nodeFormat;
                videoNode = factory->createNode(nodeFormat);
                if (videoNode) {
                    qCDebug(qLcVideo) << "updatePaintNode: Video node created. Handle type:" << m_frame.handleType()
                                     << " Supported formats for the handle by this node:"
                                     << factory->supportedPixelFormats(m_frame.handleType());
                    break;
                }
            }
        }
    }

    if (!videoNode) {
        m_frameChanged = false;
        m_frame = QVideoFrame();
        return nullptr;
    }

    // Negative rotations need lots of %360
    videoNode->setTexturedRectGeometry(m_renderedRect, m_sourceTextureRect,
                                       qNormalizedOrientation(q->orientation()));
    if (m_frameChanged) {
        QSGVideoNode::FrameFlags flags;
        if (isFrameModified)
            flags |= QSGVideoNode::FrameFiltered;
        videoNode->setCurrentFrame(m_frame, flags);

        if ((q->flushMode() == QDeclarativeVideoOutput::FirstFrame && !m_frameOnFlush.isValid())
            || q->flushMode() == QDeclarativeVideoOutput::LastFrame) {
            m_frameOnFlush = m_surfaceFormat.handleType() == QVideoFrame::NoHandle
                ? m_frame
                : m_frame.image();
        }

        //don't keep the frame for more than really necessary
        m_frameChanged = false;
        m_frame = QVideoFrame();
    }
    return videoNode;
}

QAbstractVideoSurface *QDeclarativeVideoBackend::videoSurface() const
{
    return m_surface;
}

QRectF QDeclarativeVideoBackend::adjustedViewport() const
{
    return m_surfaceFormat.viewport();
}

#if QT_CONFIG(opengl)
QOpenGLContext *QDeclarativeVideoBackend::glContext() const
{
    return m_glContext;
}
#endif

void QDeclarativeVideoBackend::present(const QVideoFrame &frame)
{
    m_frameMutex.lock();
    m_frame = frame.isValid() ? frame : m_frameOnFlush;
    m_frameChanged = true;
    m_frameMutex.unlock();

    q->update();
}

void QDeclarativeVideoBackend::stop()
{
    present(QVideoFrame());
}

QSGVideoItemSurface::QSGVideoItemSurface(QDeclarativeVideoBackend *backend, QObject *parent)
    : QAbstractVideoSurface(parent),
      m_backend(backend)
{
}

QSGVideoItemSurface::~QSGVideoItemSurface() = default;

QList<QVideoFrame::PixelFormat> QSGVideoItemSurface::supportedPixelFormats(
        QVideoFrame::HandleType handleType) const
{
    QList<QVideoFrame::PixelFormat> formats;

    static bool noGLTextures = false;
    static bool noGLTexturesChecked = false;
    if (handleType == QVideoFrame::GLTextureHandle) {
        if (!noGLTexturesChecked) {
            noGLTexturesChecked = true;
            noGLTextures = qEnvironmentVariableIsSet("QT_QUICK_NO_TEXTURE_VIDEOFRAMES");
        }
        if (noGLTextures)
            return formats;
    }

    for (QSGVideoNodeFactoryInterface* factory : qAsConst(m_backend->m_videoNodeFactories))
        formats.append(factory->supportedPixelFormats(handleType));

    return formats;
}

bool QSGVideoItemSurface::start(const QVideoSurfaceFormat &format)
{
    qCDebug(qLcVideo) << "Video surface format:" << format << "all supported formats:" << supportedPixelFormats(format.handleType());
    m_backend->m_frameOnFlush = QVideoFrame();

    if (!supportedPixelFormats(format.handleType()).contains(format.pixelFormat()))
        return false;

    m_backend->m_surfaceFormat = format;
    return QAbstractVideoSurface::start(format);
}

void QSGVideoItemSurface::stop()
{
    m_backend->stop();
    QAbstractVideoSurface::stop();
}

bool QSGVideoItemSurface::present(const QVideoFrame &frame)
{
    m_backend->present(frame);
    return true;
}

#if QT_CONFIG(opengl)
void QSGVideoItemSurface::scheduleOpenGLContextUpdate()
{
    //This method is called from render thread
    QMetaObject::invokeMethod(this, "updateOpenGLContext");
}

void QSGVideoItemSurface::updateOpenGLContext()
{
    //Set a dynamic property to access the OpenGL context in Qt Quick render thread.
    this->setProperty("GLContext", QVariant::fromValue<QObject*>(m_backend->glContext()));
}
#endif

QT_END_NAMESPACE
