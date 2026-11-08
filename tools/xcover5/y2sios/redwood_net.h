/*
 * From y2sios (https://github.com/stormtroopxyz/y2sios, kernel/xnu/files/), MIT License,
 * Copyright (c) 2026 the y2sios contributors; itself after HackiOS by OOSEX GANG (MIT). Taken for the XCover5 port's
 * Wi-Fi (bsd/dev/xc5_wlbt.c supplies the station side); changes marked XCover5.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and
 * this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS
 * PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 * XCover5: of y2sios's sys/redwood_net.h only what bsd/dev/redwood_apple80211.c needs: the interface itself is made in
 * bsd/dev/xc5_wlbt.c, which hands an Apple80211 request (SIOCGA80211, SIOCSA80211) for its interface over to this.
 */
#ifndef _SYS_REDWOOD_NET_H_
#define _SYS_REDWOOD_NET_H_
#ifdef KERNEL
#include <sys/types.h>
errno_t redwood_apple80211_ioctl(u_long command, void *data);
#endif
#endif