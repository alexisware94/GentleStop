#pragma once
typedef int ID;
ID register_named_variable(const char* name);
double get_named_variable_value(ID id);
void set_named_variable_value(ID id, double v);
