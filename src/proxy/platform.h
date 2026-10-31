// Camada mínima de plataforma: carregar a DLL real do driver e descobrir onde
// a nossa própria DLL está. Windows é o alvo; o caminho POSIX existe só para
// os testes automatizados rodarem no Linux.
#pragma once

#include <string>

namespace gf {

using LibHandle = void *;

// Caminho completo da DLL/SO que contém este código.
std::string OwnModulePath();

// Caminho da DLL real do Pico: "<nosso_nome>_real.<ext>" na mesma pasta.
// Ex.: driver_pico.dll -> driver_pico_real.dll
std::string RealDriverPath();

LibHandle LoadLib(const std::string &path, std::string *error);
void *GetSymbol(LibHandle lib, const char *name);

// Tempo monotônico em segundos (alta resolução).
double NowSeconds();

} // namespace gf
