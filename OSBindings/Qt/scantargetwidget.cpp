#include "scantargetwidget.h"

#include <QDebug>
#include <QOpenGLContext>

ScanTargetWidget::ScanTargetWidget(QWidget *parent) : QOpenGLWidget(parent) {}
ScanTargetWidget::~ScanTargetWidget() {}

void ScanTargetWidget::initializeGL() {
	glClearColor(0.5, 0.5, 1.0, 1.0);

	qDebug() << "share context: " << bool(context()->shareGroup());
}

void ScanTargetWidget::paintGL() {
	glClear(GL_COLOR_BUFFER_BIT);
	if(scanTarget) {
		scanTarget->update(width(), height());
		scanTarget->draw(width(), height());
	}
}

void ScanTargetWidget::resizeGL(int w, int h) {
    glViewport(0,0,w,h);
}

Outputs::Display::OpenGL::ScanTarget *ScanTargetWidget::getScanTarget() {
	makeCurrent();
	if(!scanTarget) {
		scanTarget = std::make_unique<Outputs::Display::OpenGL::ScanTarget>(defaultFramebufferObject());
	}
	return scanTarget.get();
}
