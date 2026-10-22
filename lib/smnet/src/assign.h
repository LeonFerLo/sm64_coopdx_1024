/* Asignación de costo mínimo con capacidades (PROTOCOL.md §9.2).
 *
 * n peers, m supernodos (m chico, típicamente ≤ 64). Cada peer va a un SN o a ninguno.
 * Costo total = Σ cost[p][s] + Σ_s beta · (0 + 1 + ... + (load_s − 1)) / cap_s
 * (costo marginal creciente por ocupación: reparte la carga).
 *
 * Algoritmo: caminos de aumento más cortos (successive shortest paths). Se inserta un peer
 * por vez y se busca el camino más barato a un SN con lugar, permitiendo mover a otros peers
 * de SN en cadena. Como los SN son pocos, el camino se busca con Bellman-Ford sobre m nodos,
 * con aristas s→t = el peer de s que menos cuesta mover a t. Es exacto (mantiene optimalidad
 * en cada inserción) y cuesta O(n·(n·m + m³)) en el peor caso: ~50 ms para 1024 × 26. */
#ifndef SMNET_ASSIGN_H
#define SMNET_ASSIGN_H

#define SMNET_ASSIGN_INF 1e8f

/* cost: matriz n×m por filas (cost[p*m + s]); valores >= SMNET_ASSIGN_INF = no factible.
 * cap: capacidad de cada SN. out: SN asignado a cada peer o -1. Devuelve cuántos asignó. */
int smnet_assign_solve(int n, int m, const float* cost, const int* cap, float beta, int* out);

/* Costo total de una asignación (para tests y para comparar con la anterior). */
double smnet_assign_total(int n, int m, const float* cost, const int* cap, float beta, const int* out);

#endif
