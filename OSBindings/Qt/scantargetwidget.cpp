#include "scantargetwidget.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QScreen>
#include <QTimer>

#include "../../ClockReceiver/TimeTypes.hpp"

ScanTargetWidget::ScanTargetWidget(QWidget *parent) : QOpenGLWidget(parent) {}
ScanTargetWidget::~ScanTargetWidget() {}

void ScanTargetWidget::initializeGL() {
	setDefaultClearColour();

	// Follow each swapped frame with an additional update.
	connect(this, &QOpenGLWidget::frameSwapped, this, &ScanTargetWidget::vsync);
}

void ScanTargetWidget::paintGL() {
	if(requested_redraw_time_) {
		const auto now = Time::nanos_now();
		vsyncPredictor.add_timer_jitter(now - requested_redraw_time_);
		requested_redraw_time_ = 0;
	}

	const float newOutputScale = float(window()->screen()->devicePixelRatio());
	if(outputScale != newOutputScale) {
		outputScale = newOutputScale;
		resize();
	}
	vsyncPredictor.set_frame_rate(float(window()->screen()->refreshRate()));

	glClear(GL_COLOR_BUFFER_BIT);

	// Gmynastics ahoy: if a producer has been specified or previously connected then:
	//
	//	(i) if it's a new producer, generate a new scan target and pass it on;
	//	(ii) in any case, check whether the underlyiung framebuffer has changed; and
	//	(iii) draw.
	//
	// The slightly convoluted scan target forwarding arrangement works around an issue
	// with QOpenGLWidget under macOS, which I did not fully diagnose, in which creating
	// a scan target in ::initializeGL did not work (and no other arrangement really works
	// with regard to starting up).
	if(isConnected || producer) {
		if(producer) {
			isConnected = true;
			framebuffer = defaultFramebufferObject();
			scanTarget = std::make_unique<Outputs::Display::OpenGL::ScanTarget>(framebuffer);
			producer->set_scan_target(scanTarget.get());
			producer = nullptr;
		}

		// Qt reserves the right to change the framebuffer object due to window resizes or if setParent is called;
		// therefore check whether it has changed.
		const auto newFramebuffer = defaultFramebufferObject();
		if(framebuffer != newFramebuffer) {
			framebuffer = newFramebuffer;
			scanTarget->set_target_framebuffer(framebuffer);
		}

		vsyncPredictor.begin_redraw();
		scanTarget->update(scaledWidth, scaledHeight);
		scanTarget->draw(scaledWidth, scaledHeight);
		glFinish();	// Make sure all costs are properly accounted for in the vsync predictor.
		vsyncPredictor.end_redraw();
	}
}

void ScanTargetWidget::vsync() {
	if(!isConnected) return;

	vsyncPredictor.announce_vsync();

	const auto time_now = Time::nanos_now();
	requested_redraw_time_ = vsyncPredictor.suggested_draw_time();
	const auto delay_time = (requested_redraw_time_ - time_now) / 1'000'000;
	if(delay_time > 0) {
		QTimer::singleShot(delay_time, this, SLOT(repaint()));
	} else {
		requested_redraw_time_ = 0;
		repaint();
	}
}

void ScanTargetWidget::resizeGL(int w, int h) {
	if(width != w || height != h) {
		width = w;
		height = h;
		resize();
	}
}

void ScanTargetWidget::resize() {
	const int newScaledWidth = int(float(width) * outputScale);
	const int newScaledHeight = int(float(height) * outputScale);

	if(newScaledWidth != scaledWidth || newScaledHeight != scaledHeight) {
		scaledWidth = newScaledWidth;
		scaledHeight = newScaledHeight;
		glViewport(0, 0, scaledWidth, scaledHeight);
	}
}

void ScanTargetWidget::setScanProducer(MachineTypes::ScanProducer *producer) {
	this->producer = producer;
	repaint();
}

void ScanTargetWidget::stop() {
	makeCurrent();
	scanTarget.reset();
	isConnected = false;
	setDefaultClearColour();
	vsyncPredictor.pause();
	requested_redraw_time_ = 0;
}

void ScanTargetWidget::setDefaultClearColour() {
	// Retain the default background colour.
	const QColor backgroundColour = palette().color(QWidget::backgroundRole());
	glClearColor(backgroundColour.redF(), backgroundColour.greenF(), backgroundColour.blueF(), 1.0);
}
