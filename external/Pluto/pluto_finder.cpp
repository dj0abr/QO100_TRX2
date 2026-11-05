/*
* Adalm Pluto Driver
* ==================
* Author: DJ0ABR
*
*   (c) DJ0ABR
*   www.dj0abr.de
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
* 
* ==========================================
* find a pluto connected via USB or Ethernet
* ==========================================
* 
*/

#include "pluto.h"

char pluto_context_name[50];

int pluto_get_IP(char *url_IP)
{
    // === connect Pluto at a given IP address ===
    struct sockaddr_in sa;
    
    int res = inet_pton(AF_INET,url_IP+3,&(sa.sin_addr));
    if(res == 1)
    {
        // we have a valid pluto IP continue using this IP
        sprintf(pluto_context_name,"%s",url_IP);
        //printf("searching Pluto on %s\n",pluto_context_name);
        return 1;
    }
    //printf("cannot evaluate %s\n",url_IP);
    return 0;
}

int pluto_get_USB(char *sn)
{
    // Scan USB directly through libiio; no dependency on iio_info text output.
    iio_scan_context *scan=iio_create_scan_context("usb",0);
    if(!scan) return 0;
    iio_context_info **list=NULL;
    const ssize_t count=iio_scan_context_get_info_list(scan,&list);
    bool found=false;
    for(ssize_t i=0;i<count && !found;++i) {
        const char *uri=iio_context_info_get_uri(list[i]);
        const char *description=iio_context_info_get_description(list[i]);
        if(!uri || strncmp(uri,"usb:",4) || !description ||
           (!strstr(description,"Pluto") && !strstr(description,"PLUTO")) ||
           (*sn && !strstr(description,sn))) continue;
        if(strlen(uri)>=sizeof(pluto_context_name)) continue;
        strcpy(pluto_context_name,uri);found=true;
        printf("PLUTO found: %s [%s]\n",description,uri);
    }
    if(list) iio_context_info_list_free(list);
    iio_scan_context_destroy(scan);
    return found?1:0;
}
