#include "TaskbarComponent.h"

#if 0 // QtWinExtras gibt es unter Qt 6 nicht mehr
#include "TaskbarComponentWin.h"
#endif

/////////////////////////////////////////////////////////////////////////////////////////
TaskbarComponent& TaskbarComponent::Get()
{
#if 0 // QtWinExtras gibt es unter Qt 6 nicht mehr
  static TaskbarComponentWin instance;
  return instance;
#else
  qWarning() << "Could not find a taskbar component matching this platform. Taskbar functions disabled.";

  static TaskbarComponent instance;
  return instance;
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponent::setWindow(QQuickWindow* window)
{
  m_window = window;
}
