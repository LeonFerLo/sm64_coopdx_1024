#pragma once
#include <cstdint>

/* Encuentro UDP para las partidas masivas (smnet). Ver rendezvous.cpp y MASIVO.md. */
bool rendezvous_begin(uint16_t aPortA, uint16_t aPortB);
