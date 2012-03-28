/***

pktsize.c	- the packet size breakdown facility

Written by Gerard Paul Java
Copyright (c) Gerard Paul Java 1997-1999

This software is open-source; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License in the included COPYING file for
details.

***/

#include "iptraf-ng-compat.h"

#include "attrs.h"
#include "dirs.h"
#include "fltdefs.h"
#include "fltselect.h"
#include "isdntab.h"
#include "ifaces.h"
#include "packet.h"
#include "deskman.h"
#include "error.h"
#include "pktsize.h"
#include "options.h"
#include "timer.h"
#include "instances.h"
#include "log.h"
#include "logvars.h"
#include "promisc.h"

extern int exitloop;
extern int daemonized;

extern void write_size_log(struct ifstat_brackets *brackets,
			   unsigned long interval, char *ifname,
			   unsigned int mtu, FILE * logfile);

void rotate_size_log(int s)
{
	rotate_flag = 1;
	strcpy(target_logname, current_logfile);
	signal(SIGUSR1, rotate_size_log);
}

int initialize_brackets(char *ifname, struct ifstat_brackets *brackets,
			unsigned int *interval, unsigned int *mtu, WINDOW * win)
{
	struct ifreq ifr;
	int istat;
	int i;

	strcpy(ifr.ifr_name, ifname);

	int fd = xsocket_raw_eth_p_all();

	istat = ioctl(fd, SIOCGIFMTU, &ifr);

	close(fd);
	if (istat < 0) {
		write_error("Unable to obtain interface MTU");
		return 1;
	}
	*interval = ifr.ifr_mtu / 20;	/* There are 20 packet size brackets */

	for (i = 0; i <= 19; i++) {
		brackets[i].floor = *interval * i + 1;
		brackets[i].ceil = *interval * (i + 1);
		brackets[i].count = 0;
	}

	brackets[19].ceil = ifr.ifr_mtu;

	for (i = 0; i <= 9; i++) {
		wattrset(win, STDATTR);
		wmove(win, i + 5, 2);
		wprintw(win, "%4u to %4u:", brackets[i].floor,
			brackets[i].ceil);
		wmove(win, i + 5, 23);
		wattrset(win, HIGHATTR);
		wprintw(win, "%8lu", 0);
	}

	for (i = 10; i <= 19; i++) {
		wattrset(win, STDATTR);
		wmove(win, (i - 10) + 5, 36);

		if (i != 19)
			wprintw(win, "%4u to %4u:", brackets[i].floor,
				brackets[i].ceil);
		else
			wprintw(win, "%4u to %4u+:", brackets[i].floor,
				brackets[i].ceil);

		wmove(win, (i - 10) + 5, 57);
		wattrset(win, HIGHATTR);
		wprintw(win, "%8lu", 0);
	}

	wattrset(win, STDATTR);
	mvwprintw(win, 17, 1,
		  "Interface MTU is %d bytes, not counting the data-link header",
		  ifr.ifr_mtu);
	mvwprintw(win, 18, 1,
		  "Maximum packet size is the MTU plus the data-link header length");
	mvwprintw(win, 19, 1,
		  "Packet size computations include data-link headers, if any");

	*mtu = ifr.ifr_mtu;
	return 0;
}

void update_size_distrib(unsigned int length, struct ifstat_brackets *brackets,
			 unsigned int interval, WINDOW * win)
{
	unsigned int i;

	i = (length - 1) / interval;	/* minus 1 to keep interval
					   boundary lengths within the
					   proper brackets */

	if (i > 19)		/* This is for extras for MTU's not */
		i = 19;		/* divisible by 20 */

	brackets[i].count++;

	if (i < 10)
		wmove(win, i + 5, 23);
	else
		wmove(win, (i - 10) + 5, 57);

	wprintw(win, "%8lu", brackets[i].count);
}

void packet_size_breakdown(struct OPTIONS *options, char *ifname,
			   int facilitytime, struct filterstate *ofilter)
{
	WINDOW *win;
	PANEL *panel;
	WINDOW *borderwin;
	PANEL *borderpanel;

	struct ifstat_brackets brackets[20];
	unsigned int interval;

	int ch;

	char buf[MAX_PACKET_SIZE];
	int br;
	char *ipacket;
	char iface[IFNAMSIZ];
	unsigned int mtu;

	struct sockaddr_ll fromaddr;
	int pkt_result;

	struct timeval tv;
	unsigned long starttime, startlog, timeint;
	unsigned long now;
	unsigned long long unow;
	unsigned long updtime = 0;
	unsigned long long updtime_usec = 0;

	int logging = options->logging;
	FILE *logfile = NULL;

	struct promisc_states *promisc_list;

	char msgstring[80];
	int fd;

	if (!facility_active(PKTSIZEIDFILE, ifname))
		mark_facility(PKTSIZEIDFILE, "Packet size breakdown", ifname);
	else {
		snprintf(msgstring, 80,
			 "Packet sizes already being monitored on %s", ifname);
		write_error(msgstring);
		return;
	}

	if (!iface_up(ifname)) {
		err_iface_down();
		unmark_facility(PKTSIZEIDFILE, ifname);
		return;
	}
	borderwin = newwin(LINES - 2, COLS, 1, 0);
	borderpanel = new_panel(borderwin);

	wattrset(borderwin, BOXATTR);
	tx_box(borderwin, ACS_VLINE, ACS_HLINE);
	mvwprintw(borderwin, 0, 1, " Packet Distribution by Size ");

	win = newwin(LINES - 4, COLS - 2, 2, 1);
	panel = new_panel(win);

	tx_stdwinset(win);
	wtimeout(win, -1);
	wattrset(win, STDATTR);
	tx_colorwin(win);

	move(LINES - 1, 1);
	stdexitkeyhelp();

	initialize_brackets(ifname, brackets, &interval, &mtu, win);

	mvwprintw(win, 1, 1, "Packet size brackets for interface %s", ifname);
	wattrset(win, BOXATTR);

	mvwprintw(win, 4, 1, "Packet Size (bytes)");
	mvwprintw(win, 4, 26, "Count");
	mvwprintw(win, 4, 36, "Packet Size (bytes)");
	mvwprintw(win, 4, 60, "Count");
	wattrset(win, HIGHATTR);

	if (logging) {
		if (strcmp(current_logfile, "") == 0) {
			snprintf(current_logfile, 80, "%s-%s.log", PKTSIZELOG,
				 ifname);

			if (!daemonized)
				input_logfile(current_logfile, &logging);
		}
	}

	if (logging) {
		opentlog(&logfile, current_logfile);

		if (logfile == NULL)
			logging = 0;
	}
	if (logging)
		signal(SIGUSR1, rotate_size_log);

	writelog(logging, logfile,
		 "******** Packet size distribution facility started ********");

	exitloop = 0;
	gettimeofday(&tv, NULL);
	now = starttime = startlog = timeint = tv.tv_sec;

	if ((first_active_facility()) && (options->promisc)) {
		init_promisc_list(&promisc_list);
		save_promisc_list(promisc_list);
		srpromisc(1, promisc_list);
		destroy_promisc_list(&promisc_list);
	}

	adjust_instance_count(PROCCOUNTFILE, 1);

	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if(fd == -1) {
		write_error("Unable to obtain monitoring socket");
		goto err;
	}
	if(socket_bind_to_iface_by_name(fd, ifname) == -1) {
		write_error("Unable to bind interface on the socket");
		goto err_close;
	}

	do {
		gettimeofday(&tv, NULL);
		now = tv.tv_sec;
		unow = tv.tv_sec * 1000000ULL + tv.tv_usec;

		if (((options->updrate != 0)
		     && (now - updtime >= options->updrate))
		    || ((options->updrate == 0)
			&& (unow - updtime_usec >= DEFAULT_UPDATE_DELAY))) {
			update_panels();
			doupdate();
			updtime = now;
			updtime_usec = unow;
		}
		if (now - timeint >= 5) {
			printelapsedtime(starttime, now, LINES - 3, 1,
					 borderwin);
			timeint = now;
		}
		if ((now - startlog >= options->logspan) && (logging)) {
			write_size_log(brackets, now - starttime, ifname, mtu,
				       logfile);
			startlog = now;
		}
		check_rotate_flag(&logfile, logging);

		if ((facilitytime != 0)
		    && (((now - starttime) / 60) >= facilitytime))
			exitloop = 1;

		getpacket(fd, buf, &fromaddr, &ch, &br, iface, win);

		if (ch != ERR) {
			switch (ch) {
			case 12:
			case 'l':
			case 'L':
				tx_refresh_screen();
				break;
			case 'x':
			case 'X':
			case 'q':
			case 'Q':
			case 27:
			case 24:
				exitloop = 1;
			}
		}
		if (br > 0) {
			pkt_result =
			    processpacket(buf, &ipacket, (unsigned int *) &br,
					  NULL, NULL, NULL, &fromaddr,
					  ofilter,
					  MATCH_OPPOSITE_USECONFIG, iface,
					  0);

			if (pkt_result != PACKET_OK)
				continue;

			update_size_distrib(br, brackets, interval, win);
		}
	} while (!exitloop);

err_close:
	close(fd);
err:
	if (logging) {
		signal(SIGUSR1, SIG_DFL);
		write_size_log(brackets, now - starttime, ifname, mtu, logfile);
		writelog(logging, logfile,
			 "******** Packet size distribution facility stopped ********");
		fclose(logfile);
	}

	if ((options->promisc) && (is_last_instance())) {
		load_promisc_list(&promisc_list);
		srpromisc(0, promisc_list);
		destroy_promisc_list(&promisc_list);
	}

	adjust_instance_count(PROCCOUNTFILE, -1);

	del_panel(panel);
	delwin(win);
	del_panel(borderpanel);
	delwin(borderwin);
	unmark_facility(PKTSIZEIDFILE, ifname);
	strcpy(current_logfile, "");
}
