#include <stdlib.h>
#include <string.h>
#include "assign.h"

#define INF SMNET_ASSIGN_INF

double smnet_assign_total(int n, int m, const float* cost, const int* cap, float beta, const int* out) {
    double total = 0;
    int* load = calloc((size_t)m, sizeof(int));
    for (int p = 0; p < n; p++) {
        if (out[p] < 0) continue;
        total += cost[p * m + out[p]];
        load[out[p]]++;
    }
    for (int s = 0; s < m; s++)
        if (cap[s] > 0) total += beta * (double)load[s] * (load[s] - 1) / 2.0 / cap[s];
    free(load);
    return total;
}

/* Núcleo: m0 SN reales + 1 columna ficticia "sin asignar" (costo PEN, capacidad n, sin costo
 * marginal). Con la columna ficticia el problema siempre es factible y los caminos de aumento
 * pueden desalojar a un peer caro para meter a uno barato cuando no hay lugar para todos. */
#define PEN 1e6f
int smnet_assign_solve(int n, int m0, const float* cost0, const int* cap0, float beta, int* out) {
    int m = m0 + 1;
    float* cost = malloc(sizeof(float) * (size_t)(n * m));
    int* cap = malloc(sizeof(int) * (size_t)m);
    for (int p = 0; p < n; p++) {
        for (int s = 0; s < m0; s++) cost[p * m + s] = cap0[s] > 0 ? cost0[p * m0 + s] : INF;
        cost[p * m + m0] = PEN;
    }
    for (int s = 0; s < m0; s++) cap[s] = cap0[s];
    cap[m0] = n;

    int* load = calloc((size_t)m, sizeof(int));
    float* D = malloc(sizeof(float) * (size_t)(m * m));       /* D[s*m+t]: costo de mover un peer de s a t */
    int* Darg = malloc(sizeof(int) * (size_t)(m * m));
    float* dist = malloc(sizeof(float) * (size_t)m);
    int* pred = malloc(sizeof(int) * (size_t)m);
    int* pathbuf = malloc(sizeof(int) * (size_t)(m + 1));
    for (int p = 0; p < n; p++) out[p] = -1;

    for (int p = 0; p < n; p++) {
        /* aristas entre columnas: el peer de s más barato de mover a t */
        for (int k = 0; k < m * m; k++) { D[k] = INF; Darg[k] = -1; }
        for (int q = 0; q < p; q++) {
            int s = out[q];
            float cs = cost[q * m + s];
            for (int t = 0; t < m; t++) {
                if (t == s) continue;
                float ct = cost[q * m + t];
                if (ct >= INF) continue;
                float d = ct - cs;
                if (d < D[s * m + t]) { D[s * m + t] = d; Darg[s * m + t] = q; }
            }
        }
        /* Bellman-Ford desde el peer nuevo */
        for (int s = 0; s < m; s++) { dist[s] = cost[p * m + s]; pred[s] = -1; }
        for (int it = 0; it < m - 1; it++) {
            int changed = 0;
            for (int s = 0; s < m; s++) {
                if (dist[s] >= INF) continue;
                for (int t = 0; t < m; t++) {
                    float d = D[s * m + t];
                    if (d >= INF) continue;
                    if (dist[s] + d < dist[t] - 1e-4f) { dist[t] = dist[s] + d; pred[t] = s; changed = 1; }
                }
            }
            if (!changed) break;
        }
        /* destino: columna con lugar que minimiza camino + costo marginal de ocupación */
        int best = -1;
        float bv = INF;
        for (int t = 0; t < m; t++) {
            if (dist[t] >= INF || load[t] >= cap[t]) continue;
            float marg = t < m0 ? beta * (float)load[t] / (float)cap[t] : 0.0f;
            float v = dist[t] + marg;
            if (v < bv) { bv = v; best = t; }
        }
        /* siempre hay destino: la columna ficticia tiene lugar */
        int len = 0, t = best;
        while (t >= 0 && len <= m) { pathbuf[len++] = t; t = pred[t]; }
        if (len > m) { out[p] = m0; load[m0]++; continue; }   /* ciclo por redondeo: no debería pasar */
        for (int k = 0; k + 1 < len; k++) {
            int to = pathbuf[k], from = pathbuf[k + 1];
            out[Darg[from * m + to]] = to;
        }
        out[p] = pathbuf[len - 1];
        load[best]++;
    }
    int assigned = 0;
    for (int p = 0; p < n; p++) {
        if (out[p] == m0) out[p] = -1;
        else assigned++;
    }
    free(cost); free(cap); free(load); free(D); free(Darg); free(dist); free(pred); free(pathbuf);
    return assigned;
}
