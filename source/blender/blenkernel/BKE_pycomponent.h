#ifndef BKE_PYCOMPONENT_H
#define BKE_PYCOMPONENT_H

#ifdef __cplusplus
extern "C" {
#endif

struct PythonComponent *new_component_from_import(char *import);
void free_component(struct PythonComponent *pc);
void free_components(struct ListBase *base);

#ifdef __cplusplus
}
#endif

#endif // BKE_PYCOMPONENT_H
