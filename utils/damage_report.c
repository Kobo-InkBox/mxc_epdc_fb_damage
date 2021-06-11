/*
	damage_report: Simple example for mxc_epdc_fb_damage usage.
	Copyright (C) 2021 NiLuJe <ninuje@gmail.com>
	SPDX-License-Identifier: GPL-2.0-only

	----

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../mxc_epdc_fb_damage.h"

int main(void) {
	int fd = open("/dev/fbdamage", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd == -1) {
		perror("open");
		return EXIT_FAILURE;
	}

	struct pollfd pfd = { 0 };
	pfd.fd = fd;
	pfd.events = POLLIN;
	while (poll(&pfd, 1, -1) >= 0) {
		mxcfb_damage_update damage = { 0 };

		while (read(fd, &damage, sizeof(damage)) == sizeof(damage)) {
			printf("MXCFB_SEND_UPDATE_V1_NTX: overflow_notify=%d {update_region={top=%u, left=%u, width=%u, height=%u}, waveform_mode=%u, update_mode=%u, update_marker=%u, temp=%d, flags=%u, alt_buffer_data={virt_addr=%p, phys_addr=%u, width=%u, height=%u, alt_update_region={top=%u, left=%u, width=%u, height=%u}}}\n",
			damage.overflow_notify,
			damage.data.update_region.top,
			damage.data.update_region.left,
			damage.data.update_region.width,
			damage.data.update_region.height,
			damage.data.waveform_mode,
			damage.data.update_mode,
			damage.data.update_marker,
			damage.data.temp,
			damage.data.flags,
			damage.data.alt_buffer_data.virt_addr,
			damage.data.alt_buffer_data.phys_addr,
			damage.data.alt_buffer_data.width,
			damage.data.alt_buffer_data.height,
			damage.data.alt_buffer_data.alt_update_region.top,
			damage.data.alt_buffer_data.alt_update_region.left,
			damage.data.alt_buffer_data.alt_update_region.width,
			damage.data.alt_buffer_data.alt_update_region.height);
		}
	}

	return EXIT_SUCCESS; // Unreachable
}
