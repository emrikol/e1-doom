#ifndef E1_DASHBOARD_H
#define E1_DASHBOARD_H

/* Camera builds reinterpret the Doom menu's Quit action as a request to
 * return to the live camera.  The injected transition worker performs the
 * actual melt and remains responsible for every media mutation. */
void I_E1ReturnToCamera(void);

#endif
