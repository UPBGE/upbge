/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_System.hh"

#include "GHOST_EventManager.hh"
#include "GHOST_TimerManager.hh"
#include "GHOST_TimerTask.hh"
#include "GHOST_WindowManager.hh"

#ifdef WITH_INPUT_NDOF
#  include "GHOST_NDOFManager.hh"
#endif

GHOST_System::GHOST_System()
    : m_nativePixel(false),
      m_windowFocus(true),
      m_autoFocus(true),
      m_timerManager(nullptr),
      m_windowManager(nullptr),
      m_eventManager(nullptr),
#ifdef WITH_INPUT_NDOF
      m_ndofManager(nullptr),
#endif
      m_multitouchGestures(true),
      m_tabletAPI(GHOST_kTabletAutomatic),
      m_is_debug_enabled(false)
{
}

GHOST_System::~GHOST_System()
{
  exit();
}

GHOST_TSuccess GHOST_System::hasClipboardImage() const
{
  return GHOST_kFailure;
}

uint *GHOST_System::getClipboardImage(int * /*r_width*/, int * /*r_height*/) const
{
  return nullptr;
}

GHOST_TSuccess GHOST_System::putClipboardImage(uint * /*rgba*/,
                                               int /*width*/,
                                               int /*height*/) const
{
  return GHOST_kFailure;
}

GHOST_ITimerTask *GHOST_System::installTimer(uint64_t delay,
                                             uint64_t interval,
                                             GHOST_TimerProcPtr timerProc,
                                             GHOST_TUserDataPtr userData)
{
  uint64_t millis = getMilliSeconds();
  GHOST_TimerTask *timer = new GHOST_TimerTask(millis + delay, interval, timerProc, userData);
  if (timer) {
    if (m_timerManager->addTimer(timer) == GHOST_kSuccess) {
      /* Check to see whether we need to fire the timer right away. */
      m_timerManager->fireTimers(millis);
    }
    else {
      delete timer;
      timer = nullptr;
    }
  }
  return timer;
}

GHOST_TSuccess GHOST_System::removeTimer(GHOST_ITimerTask *timerTask)
{
  GHOST_TSuccess success = GHOST_kFailure;
  if (timerTask) {
    success = m_timerManager->removeTimer((GHOST_TimerTask *)timerTask);
  }
  return success;
}

GHOST_TSuccess GHOST_System::disposeWindow(GHOST_IWindow *window)
{
  GHOST_TSuccess success;

  /*
   * Remove all pending events for the window.
   */
  if (m_windowManager->getWindowFound(window)) {
    m_eventManager->removeWindowEvents(window);  
    success = m_windowManager->removeWindow(window);
    if (success) {
      delete window;
    }
  }
  else {
    if (window == m_windowManager->getFullScreenWindow()) {
      success = endFullScreen();
    }
    else if (m_windowManager->getWindowFound(window)) {
      success = m_windowManager->removeWindow(window);
      if (success) {
        delete window;
      }
    }
    else {
      success = GHOST_kFailure;
    }
  }
  return success;
}

bool GHOST_System::validWindow(GHOST_IWindow *window)
{
  return m_windowManager->getWindowFound(window);
}

GHOST_TSuccess GHOST_System::beginFullScreen(GHOST_IWindow **window,
                                             const GHOST_DisplaySettings &settings,
                                             const GHOST_GPUSettings &gpu_settings)
{
  GHOST_TSuccess success = GHOST_kFailure;
  GHOST_ASSERT(m_windowManager, "GHOST_System::beginFullScreen(): invalid window manager");
  if (!m_windowManager->getFullScreen()) {
    success = createFullScreenWindow((GHOST_Window **)window, settings, gpu_settings);
    if (success == GHOST_kSuccess) {
      m_windowManager->beginFullScreen(*window, (gpu_settings.flags & GHOST_gpuStereoVisual) != 0);
    }
    else {
      /*m_displayManager->setCurrentDisplaySetting(GHOST_DisplayManager::kMainDisplay,
                                                 m_preFullScreenSetting);*/
    }
  }
  if (success == GHOST_kFailure) {
    GHOST_PRINT("GHOST_System::beginFullScreen(): could not enter full-screen mode\n");
  }
  return success;
}

GHOST_TSuccess GHOST_System::updateFullScreen(GHOST_IWindow ** /*window*/,
                                              const GHOST_DisplaySettings & /*setting*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_System::endFullScreen()
{
  return GHOST_kSuccess;
}

bool GHOST_System::getFullScreen()
{
  bool fullScreen;
  if (m_windowManager) {
    fullScreen = m_windowManager->getFullScreen();
  }
  else {
    fullScreen = false;
  }
  return fullScreen;
}

GHOST_IWindow *GHOST_System::getWindowUnderCursor(int32_t x, int32_t y)
{
  /* TODO: This solution should follow the order of the activated windows (Z-order).
   * It is imperfect but usable in most cases. Ideally each platform should provide
   * a custom version of this function that properly considers z-order. */

  std::vector<GHOST_IWindow *> windows = m_windowManager->getWindows();
  std::vector<GHOST_IWindow *>::reverse_iterator iwindow_iter;

  /* Search through the windows in reverse order because in most cases
   * the window that is on top was created after those that are below it. */

  for (iwindow_iter = windows.rbegin(); iwindow_iter != windows.rend(); ++iwindow_iter) {

    GHOST_IWindow *win = *iwindow_iter;

    if (win->getState() == GHOST_kWindowStateMinimized) {
      continue;
    }

    GHOST_Rect bounds;
    win->getClientBounds(bounds);
    if (bounds.isInside(x, y)) {
      return win;
    }
  }

  return nullptr;
}

void GHOST_System::dispatchEvents()
{
#ifdef WITH_INPUT_NDOF
  /* NDOF Motion event is sent only once per dispatch, so do it now: */
  if (m_ndofManager) {
    m_ndofManager->sendMotionEvent();
  }
#endif

  if (m_eventManager) {
    m_eventManager->dispatchEvents();
  }

  m_timerManager->fireTimers(getMilliSeconds());
}

GHOST_TSuccess GHOST_System::addEventConsumer(GHOST_IEventConsumer *consumer)
{
  GHOST_TSuccess success;
  if (m_eventManager) {
    success = m_eventManager->addConsumer(consumer);
  }
  else {
    success = GHOST_kFailure;
  }
  return success;
}

GHOST_TSuccess GHOST_System::removeEventConsumer(GHOST_IEventConsumer *consumer)
{
  GHOST_TSuccess success;
  if (m_eventManager) {
    success = m_eventManager->removeConsumer(consumer);
  }
  else {
    success = GHOST_kFailure;
  }
  return success;
}

GHOST_TSuccess GHOST_System::pushEvent(const GHOST_IEvent *event)
{
  GHOST_TSuccess success;
  if (m_eventManager) {
    success = m_eventManager->pushEvent(event);
  }
  else {
    success = GHOST_kFailure;
  }
  return success;
}

GHOST_TSuccess GHOST_System::getCursorPositionClientRelative(const GHOST_IWindow *window,
                                                             int32_t &x,
                                                             int32_t &y) const
{
  /* Sub-classes that can implement this directly should do so. */
  int32_t screen_x, screen_y;
  GHOST_TSuccess success = getCursorPosition(screen_x, screen_y);
  if (success == GHOST_kSuccess) {
    window->screenToClient(screen_x, screen_y, x, y);
  }
  return success;
}

GHOST_TSuccess GHOST_System::setCursorPositionClientRelative(GHOST_IWindow *window,
                                                             int32_t x,
                                                             int32_t y)
{
  /* Sub-classes that can implement this directly should do so. */
  int32_t screen_x, screen_y;
  window->clientToScreen(x, y, screen_x, screen_y);
  return setCursorPosition(screen_x, screen_y);
}

uint32_t GHOST_System::getCursorPreferredLogicalSize() const
{
  return uint32_t(24);
}

GHOST_TSuccess GHOST_System::getModifierKeyState(GHOST_TModifierKey mask, bool &isDown) const
{
  GHOST_ModifierKeys keys;
  /* Get the state of all modifier keys. */
  GHOST_TSuccess success = getModifierKeys(keys);
  if (success) {
    /* Isolate the state of the key requested. */
    isDown = keys.get(mask);
  }
  return success;
}

GHOST_TSuccess GHOST_System::getButtonState(GHOST_TButton mask, bool &isDown) const
{
  GHOST_Buttons buttons;
  /* Get the state of all mouse buttons. */
  GHOST_TSuccess success = getButtons(buttons);
  if (success) {
    /* Isolate the state of the mouse button requested. */
    isDown = buttons.get(mask);
  }
  return success;
}

void GHOST_System::setMultitouchGestures(const bool use)
{
  m_multitouchGestures = use;
}

void GHOST_System::setTabletAPI(GHOST_TTabletAPI api)
{
  m_tabletAPI = api;
}

GHOST_TTabletAPI GHOST_System::getTabletAPI()
{
  return m_tabletAPI;
}

GHOST_TSuccess GHOST_System::getPixelAtCursor(float /*r_color*/[3]) const
{
  return GHOST_kFailure;
}

#ifdef WITH_INPUT_NDOF
void GHOST_System::setNDOFDeadZone(float deadzone)
{
  if (this->m_ndofManager) {
    this->m_ndofManager->setDeadZone(deadzone);
  }
}
#endif

GHOST_TSuccess GHOST_System::init()
{
  m_timerManager = new GHOST_TimerManager();
  m_windowManager = new GHOST_WindowManager();
  m_eventManager = new GHOST_EventManager();

#ifdef WITH_GHOST_DEBUG
  if (m_eventManager) {
    m_eventPrinter = new GHOST_EventPrinter();
    m_eventManager->addConsumer(m_eventPrinter);
  }
#endif /* WITH_GHOST_DEBUG */

  if (m_timerManager && m_windowManager && m_eventManager) {
    return GHOST_kSuccess;
  }
  return GHOST_kFailure;
}

GHOST_TSuccess GHOST_System::exit()
{
  if (getFullScreen()) {
    endFullScreen();
  }
  /** WARNING: exit() may run more than once, since it may need to be called from a derived class
   * destructor. Take it into account when modifying this function. */
  delete m_windowManager;
  m_windowManager = nullptr;

  delete m_timerManager;
  m_timerManager = nullptr;

  delete m_eventManager;
  m_eventManager = nullptr;

#ifdef WITH_INPUT_NDOF
  delete m_ndofManager;
  m_ndofManager = nullptr;
#endif

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_System::createFullScreenWindow(GHOST_Window **window,
                                                    const GHOST_DisplaySettings &settings,
                                                    const GHOST_GPUSettings &gpu_settings)
{
  // GHOST_PRINT("GHOST_System::createFullScreenWindow(): creating full-screen window\n");
  *window = (GHOST_Window *)createWindow("",
                                         0,
                                         0,
                                         settings.xPixels,
                                         settings.yPixels,
                                         GHOST_kWindowStateNormal,
                                         gpu_settings,
                                         true /*exclusive*/);
  return (*window == nullptr) ? GHOST_kFailure : GHOST_kSuccess;
}

bool GHOST_System::useNativePixel()
{
  m_nativePixel = true;
  return true;
}

void GHOST_System::useWindowFocus(const bool use_focus)
{
  m_windowFocus = use_focus;
}

void GHOST_System::setAutoFocus(const bool auto_focus)
{
  m_autoFocus = auto_focus;
}

void GHOST_System::initDebug(GHOST_Debug debug)
{
  m_is_debug_enabled = debug.flags & GHOST_kDebugDefault;
}

bool GHOST_System::isDebugEnabled()
{
  return m_is_debug_enabled;
}
