/* (hookshot) The F1 menu's Network tab and co-op (host/oot_coop.c), by option id: what the menu's rows say to the host
   (rx_option) and what the host says back (rx_option_set). Its own header, read by both sides, so the numbers cannot
   part. 72 on: well clear of vk_api.h's rows.
     MODE    0 off, 1 host, 2 join                      PORT    the port hosted on or joined (43400)
     ACTION  (taken once) 3 a name was typed, 4 an address: its letters come by OUT; 10 + n: go to player n
     CHAR    host -> menu, one letter of a text (0 starts one)
     TEXT    host -> menu, the text is whole: 0 the status line, 1 a note on the screen, 2 the name row, 3 the address row
     OUT     menu -> host, the next letter of a text typed in the tab (0: no more)
     PVP     players can hurt each other
     LABEL   host -> menu, where a player's name goes on the screen (oot_coop.c co_label)
     HIDE    host -> menu, 1 while one of the game's own menus is in front: the corner and the names are not drawn */
#pragma once
enum {
    HOS_OPT_COOP_MODE = 72, HOS_OPT_COOP_PORT, HOS_OPT_COOP_ACTION, HOS_OPT_COOP_CHAR, HOS_OPT_COOP_TEXT, HOS_OPT_COOP_OUT,
    HOS_OPT_COOP_PVP, HOS_OPT_COOP_LABEL, HOS_OPT_COOP_HIDE
};
