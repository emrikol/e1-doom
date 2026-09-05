#include <stddef.h>

#include "i_endoom.h"
#include "i_oalequalizer.h"
#include "i_oalsound.h"
#include "i_rumble.h"
#include "net_gui.h"
#include "net_sdl.h"

static boolean NetDisabledInit(void) { return false; }
static void NetDisabledSend(net_addr_t *addr, net_packet_t *packet)
{ (void)addr; (void)packet; }
static boolean NetDisabledRecv(net_addr_t **addr, net_packet_t **packet)
{ (void)addr; (void)packet; return false; }
static void NetDisabledAddr(net_addr_t *addr, char *buffer, int length)
{ (void)addr; if (length > 0) buffer[0] = '\0'; }
static void NetDisabledFree(net_addr_t *addr) { (void)addr; }
static net_addr_t *NetDisabledResolve(const char *addr)
{ (void)addr; return NULL; }
static void NetDisabledShutdown(void) {}

net_module_t net_sdl_module = {
    NetDisabledInit, NetDisabledInit, NetDisabledSend, NetDisabledRecv,
    NetDisabledAddr, NetDisabledFree, NetDisabledResolve, NetDisabledShutdown,
};

void NET_WaitForLaunch(void) {}
void I_Endoom(byte *data) { (void)data; }

void I_ShutdownRumble(void) {}
void I_InitRumble(void) {}
void I_CacheRumble(struct sfxinfo_s *sfx, int format, const byte *data,
                   int size, int rate)
{ (void)sfx; (void)format; (void)data; (void)size; (void)rate; }
boolean I_RumbleEnabled(void) { return false; }
boolean I_RumbleSupported(void) { return false; }
void I_RumbleMenuFeedback(void) {}
void I_UpdateRumbleEnabled(void) {}
void I_SetRumbleSupported(SDL_GameController *gamepad) { (void)gamepad; }
void I_ResetRumbleChannel(int handle) { (void)handle; }
void I_ResetAllRumbleChannels(void) {}
void I_UpdateRumble(void) {}
void I_UpdateRumbleParams(const struct mobj_s *listener,
                          const struct mobj_s *origin, int handle)
{ (void)listener; (void)origin; (void)handle; }
void I_StartRumble(const struct mobj_s *listener, const struct mobj_s *origin,
                   const struct sfxinfo_s *sfx, int handle,
                   rumble_type_t rumble_type)
{ (void)listener; (void)origin; (void)sfx; (void)handle; (void)rumble_type; }
void I_DisableRumble(void) {}
void I_BindRumbleVariables(void) {}

boolean oal_use_doppler;
void I_OAL_SetResampler(void) {}
const char **I_OAL_GetResamplerStrings(void) { return NULL; }
boolean I_OAL_EqualizerInitialized(void) { return false; }
boolean I_OAL_CustomEqualizer(void) { return false; }
void I_OAL_ShutdownEqualizer(void) {}
void I_OAL_InitEqualizer(void) {}
void I_OAL_SetEqualizer(void) {}
void I_OAL_EqualizerPreset(void) {}
void I_BindEqualizerVariables(void) {}
