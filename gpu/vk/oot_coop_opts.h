/* (hookshot) The F1 menu's Co-op tab and co-op (host/oot_coop.c), by option id: what the menu's rows say to the host
   (rx_option) and what the host says back (rx_option_set). Its own header, read by both sides, so the numbers cannot
   part. 72 on: well clear of vk_api.h's rows.
     ACTION  (taken once) 10 + n: go to the n-th of the others
     CHAR    host -> menu, one letter of a text (0 starts one)
     TEXT    host -> menu, the text is whole: 0 the status line, 1 a note on the screen, 2 the name row, 3 the address row
     PVP     players can hurt each other
     LABEL   host -> menu, where a player's name goes on the screen (oot_coop.c co_label)
     HIDE    host -> menu, 1 while one of the game's own menus is in front: the corner and the names are not drawn
   (What the room is — hosted or joined, its port, the name and address typed — is not asked this way: the tab's rows
   reach host/coop.c and host/coop_menu.c as MkwVkHost.coop_command, and co-op reads the room back from host/coop.c.) */
#pragma once
enum {
    HOS_OPT_COOP_ACTION = 72, HOS_OPT_COOP_CHAR, HOS_OPT_COOP_TEXT, HOS_OPT_COOP_PVP, HOS_OPT_COOP_LABEL, HOS_OPT_COOP_HIDE
};
