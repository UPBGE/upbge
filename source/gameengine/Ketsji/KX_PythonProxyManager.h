#pragma once

#include <vector>

class KX_GameObject;

class KX_PythonProxyManager {
 private:
  std::vector<KX_GameObject *> m_objects;
  bool m_objects_changed = false;

 public:
  KX_PythonProxyManager();
  ~KX_PythonProxyManager();

  void RegisterObject(KX_GameObject *gameobj);
  void UnregisterObject(KX_GameObject *gameobj);

  void UpdateComponents();
};
