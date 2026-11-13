# Notes for coding agents

## C++ style

- Don't put `const` on local variables of primitive or trivially copyable
  types (`int`, `bool`, `float`, `uint16_t`, `DWORD`, small POD structs, ...).
  Write `int id = f();`, not `const int id = f();`. The compiler already sees
  whether a local is modified, so the qualifier only adds noise. The same goes
  for by-value function parameters.
  This rule does not cover `const` that changes meaning: pointers and
  references to const (`const char*`, `const std::string&`), `static const`
  tables and named constants, and `constexpr`.
