#ifndef DNA_COMPONENT_TYPES_H
#define DNA_COMPONENT_TYPES_H

#include "DNA_listBase.h"

typedef struct ComponentProperty {
	struct ComponentProperty *next, *prev;
	char name[32];
	short type, pad;
	int data;
	void *ptr, *ptr2;
} ComponentProperty;

typedef struct PythonComponent {
	struct PythonComponent *next, *prev;
	ListBase properties;
	char name[64];
	char module[64];
} PythonComponent;


/* ComponentProperty.type */
#define CPROP_TYPE_INT         0
#define CPROP_TYPE_FLOAT       1
#define CPROP_TYPE_STRING      2
#define CPROP_TYPE_BOOLEAN     3
#define CPROP_TYPE_SET         4

#endif // DNA_COMPONENT_TYPES_H
