#include "gesture.h"
#include <stdlib.h>

void g_down(Gesture *g, int slot, int x, int y, long ms)
{
	if (g->down[slot]) return;
	if (g->n == 0) { g->t0 = ms; g->maxn = g->moved = g->travel = 0; g->rx = g->ry = g->rw = 0; }
	g->down[slot] = 1; g->x[slot] = x; g->y[slot] = y;
	if (++g->n > g->maxn) g->maxn = g->n;
}

void g_move(Gesture *g, int slot, int x, int y, GOut *o)
{
	if (!g->down[slot]) return;
	int dx = x - g->x[slot], dy = y - g->y[slot];
	g->x[slot] = x; g->y[slot] = y;
	if (!g->moved) { /* the first px are finger jitter: they must not move the cursor under a tap */
		g->travel += abs(dx) + abs(dy);
		if (g->travel > G_SLOP) g->moved = 1;
		return;
	}
	if (g->n == 1 && g->maxn == 1) {
		g->rx += dx * G_GAIN_NUM; g->ry += dy * G_GAIN_NUM;
		o->dx += g->rx / G_GAIN_DEN; g->rx %= G_GAIN_DEN;
		o->dy += g->ry / G_GAIN_DEN; g->ry %= G_GAIN_DEN;
	} else if (g->n == 2) { /* fingers down = wheel up, as on a laptop trackpad */
		g->rw += dy;
		o->wheel += g->rw / G_WHEEL; g->rw %= G_WHEEL;
	} /* one finger left after two: no cursor jump */
}

void g_up(Gesture *g, int slot, long ms, GOut *o)
{
	if (!g->down[slot]) return;
	g->down[slot] = 0;
	if (--g->n == 0 && !g->moved && ms - g->t0 < G_TAP_MS)
		o->click = g->maxn == 1 ? 1 : g->maxn == 2 ? 2 : 0;
}
