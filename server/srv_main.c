/**********************************************************************
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_TERMIO_H
#include <sys/termio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_WINSOCK
#include <winsock.h>
#endif

#include "capability.h"
#include "capstr.h"
#include "city.h"
#include "events.h"
#include "fcintl.h"
#include "game.h"
#include "log.h"
#include "map.h"
#include "mem.h"
#include "nation.h"
#include "netintf.h"
#include "packets.h"
#include "player.h"
#include "rand.h"
#include "registry.h"
#include "shared.h"
#include "support.h"
#include "tech.h"
#include "timing.h"
#include "version.h"

#include "autoattack.h"
#include "barbarian.h"
#include "cityhand.h"
#include "citytools.h"
#include "cityturn.h"
#include "connecthand.h"
#include "console.h"
#include "diplhand.h"
#include "gamehand.h"
#include "gamelog.h"
#include "handchat.h"
#include "mapgen.h"
#include "maphand.h"
#include "meta.h"
#include "plrhand.h"
#include "report.h"
#include "ruleset.h"
#include "sanitycheck.h"
#include "savegame.h"
#include "score.h"
#include "sernet.h"
#include "settlers.h"
#include "spacerace.h"
#include "stdinhand.h"
#include "unithand.h"
#include "unittools.h"

#include "cm.h"

#include "advdiplomacy.h"
#include "advmilitary.h"
#include "aidata.h"
#include "aihand.h"

#include "srv_main.h"


static void begin_turn(void);
static void before_end_year(void);
static void end_turn(void);
static void ai_start_turn(void);
static bool is_game_over(void);
static void save_game_auto(void);
static void generate_ai_players(void);
static int mark_nation_as_used(int nation);
static void announce_ai_player(struct player *pplayer);

static void handle_alloc_nation(struct player *pplayer,
				struct packet_alloc_nation *packet);
static void handle_turn_done(struct player *pplayer);
static void send_select_nation(struct player *pplayer);
static void srv_loop(void);

/* this is used in strange places, and is 'extern'd where
   needed (hence, it is not 'extern'd in srv_main.h) */
bool is_server = TRUE;

/* command-line arguments to server */
struct server_arguments srvarg;

/* server state information */
enum server_states server_state = PRE_GAME_STATE;
bool nocity_send = FALSE;

/* this global is checked deep down the netcode. 
   packets handling functions can set it to none-zero, to
   force end-of-tick asap
*/
bool force_end_of_sniff;


/* The next three variables make selecting nations for AI players cleaner */
static int *nations_avail;
static int *nations_used;
static int num_nations_avail;

/* this counter creates all the id numbers used */
/* use get_next_id_number()                     */
static unsigned short global_id_counter=100;
static unsigned char used_ids[8192]={0};

/* server initialized flag */
static bool has_been_srv_init = FALSE;

/**************************************************************************
...
**************************************************************************/
void srv_init(void)
{
  /* NLS init */
  init_nls();

  /* init server arguments... */

  srvarg.metaserver_no_send = DEFAULT_META_SERVER_NO_SEND;
  sz_strlcpy(srvarg.metaserver_info_line, default_meta_server_info_string());
  sz_strlcpy(srvarg.metaserver_addr, DEFAULT_META_SERVER_ADDR);
  srvarg.metaserver_port = DEFAULT_META_SERVER_PORT;

  srvarg.port = DEFAULT_SOCK_PORT;

  srvarg.loglevel = LOG_NORMAL;

  srvarg.log_filename = NULL;
  srvarg.gamelog_filename = NULL;
  srvarg.load_filename = NULL;
  srvarg.script_filename = NULL;

  srvarg.quitidle = 0;

  srvarg.extra_metaserver_info[0] = '\0';

  /* initialize teams */
  team_init();

  /* mark as initialized */
  has_been_srv_init = TRUE;

  /* done */
  return;
}

/**************************************************************************
  Returns TRUE if any one game end condition is fulfilled, FALSE otherwise
**************************************************************************/
static bool is_game_over(void)
{
  int barbs = 0, alive = 0;
  bool all_allied;
  struct player *victor = NULL;

  /* quit if we are past the year limit */
  if (game.year > game.end_year) {
    notify_conn_ex(&game.est_connections, -1, -1, E_GAME_END, 
		   _("Game ended in a draw as end year exceeded"));
    gamelog(GAMELOG_NORMAL, _("Game ended in a draw as end year exceeded"));
    return TRUE;
  }

  /* count barbarians */
  players_iterate(pplayer) {
    if (is_barbarian(pplayer)) {
      barbs++;
    }
  } players_iterate_end;

  /* the game does not quit if we are playing solo */
  if (game.nplayers == (barbs + 1)) {
    return FALSE;
  }

  /* count the living */
  players_iterate(pplayer) {
    if (pplayer->is_alive && !is_barbarian(pplayer)) {
      alive++;
      victor = pplayer;
    }
  } players_iterate_end;

  /* quit if we have team victory */
  team_iterate(pteam) {
    if (team_count_members_alive(pteam->id) == alive) {
      notify_conn_ex(&game.est_connections, -1, -1, E_GAME_END,
		     _("Team victory to %s"), pteam->name);
      gamelog(GAMELOG_NORMAL, _("Team victory to %s"), pteam->name);
      gamelog(GAMELOG_TEAM, "TEAMVICTORY %s", pteam->name);
      return TRUE;
    }
  } team_iterate_end;

  /* quit if only one player is left alive */
  if (alive == 1) {
    notify_conn_ex(&game.est_connections, -1, -1, E_GAME_END,
		   _("Game ended in victory for %s"), victor->name);
    gamelog(GAMELOG_NORMAL, _("Game ended in victory for %s"), 
        victor->name);
    gamelog(GAMELOG_TEAM, "SINGLEWINNER %s", victor->name);
    return TRUE;
  } else if (alive == 0) {
    notify_conn_ex(&game.est_connections, -1, -1, E_GAME_END, 
		   _("Game ended in a draw"));
    gamelog(GAMELOG_NORMAL, _("Game ended in a draw"));
    gamelog(GAMELOG_TEAM, "NOWINNER");
    return TRUE;
  }

  /* quit if all remaining players are allied to each other */
  all_allied = TRUE;
  players_iterate(pplayer) {
    if (!pplayer->is_alive) {
      continue;
    }
    players_iterate(aplayer) {
      if (!pplayers_allied(pplayer, aplayer) && aplayer->is_alive) {
        all_allied = FALSE;
        break;
      }
    } players_iterate_end;
    if (!all_allied) {
      break;
    }
  } players_iterate_end;
  if (all_allied) {
    notify_conn_ex(&game.est_connections, -1, -1, E_GAME_END, 
		   _("Game ended in allied victory"));
    gamelog(GAMELOG_NORMAL, _("Game ended in allied victory"));
    gamelog(GAMELOG_TEAM, "ALLIEDVICTORY");
    return TRUE;
  }

  return FALSE;
}

/**************************************************************************
  Send all information for when game starts or client reconnects.
  Ruleset information should have been sent before this.
**************************************************************************/
void send_all_info(struct conn_list *dest)
{
  conn_list_iterate(*dest, pconn) {
      send_attribute_block(pconn->player,pconn);
  }
  conn_list_iterate_end;

  send_game_info(dest);
  send_map_info(dest);
  send_player_info_c(NULL, dest);
  send_conn_info(&game.est_connections, dest);
  send_spaceship_info(NULL, dest);
  send_all_known_tiles(dest);
  send_all_known_cities(dest);
  send_all_known_units(dest);
  send_player_turn_notifications(dest);
}

/**************************************************************************
...
**************************************************************************/
static void do_apollo_program(void)
{
  struct city *pcity = find_city_wonder(B_APOLLO);

  if (pcity) {
    struct player *pplayer = city_owner(pcity);

    if (game.civstyle == 1) {
      players_iterate(other_player) {
	city_list_iterate(other_player->cities, pcity) {
	  show_area(pplayer, pcity->x, pcity->y, 0);
	} city_list_iterate_end;
      } players_iterate_end;
    } else {
      /* map_know_all will mark all unknown tiles as known and send
       * tile, unit, and city updates as necessary.  No other actions are
       * needed. */
      map_know_all(pplayer);
    }
  }
}

/**************************************************************************
...
**************************************************************************/
static void marco_polo_make_contact(void)
{
  struct city *pcity = find_city_wonder(B_MARCO);

  if (pcity) {
    players_iterate(pplayer) {
      make_contact(city_owner(pcity), pplayer, pcity->x, pcity->y);
    } players_iterate_end;
  }
}

/**************************************************************************
...
**************************************************************************/
static void update_environmental_upset(enum tile_special_type cause,
				       int *current, int *accum, int *level,
				       void (*upset_action_fn)(int))
{
  int count;

  count = 0;
  whole_map_iterate(x, y) {
    if (map_has_special(x, y, cause)) {
      count++;
    }
  } whole_map_iterate_end;

  *current = count;
  *accum += count;
  if (*accum < *level) {
    *accum = 0;
  } else {
    *accum -= *level;
    if (myrand(200) <= *accum) {
      upset_action_fn((map.xsize / 10) + (map.ysize / 10) + ((*accum) * 5));
      *accum = 0;
      *level+=4;
    }
  }

  freelog(LOG_DEBUG,
	  "environmental_upset: cause=%-4d current=%-2d level=%-2d accum=%-2d",
	  cause, *current, *level, *accum);
}

/**************************************************************************
 check for cease-fires running out; update reputation; update cancelling
 reasons
**************************************************************************/
static void update_diplomatics(void)
{
  players_iterate(player1) {
    players_iterate(player2) {
      struct player_diplstate *pdiplstate =
	  &player1->diplstates[player2->player_no];

      pdiplstate->has_reason_to_cancel =
	  MAX(pdiplstate->has_reason_to_cancel - 1, 0);

      pdiplstate->contact_turns_left =
	  MAX(pdiplstate->contact_turns_left - 1, 0);

      if(pdiplstate->type == DS_CEASEFIRE) {
	switch(--pdiplstate->turns_left) {
	case 1:
	  notify_player(player1,
			_("Game: Concerned citizens point "
  			  "out that the cease-fire with %s will run out soon."),
			player2->name);
  	  break;
  	case -1:
	  notify_player(player1,
  			_("Game: The cease-fire with %s has "
  			  "run out. You are now neutral towards the %s."),
			player2->name,
			get_nation_name_plural(player2->nation));
	  pdiplstate->type = DS_NEUTRAL;
	  check_city_workers(player1);
	  check_city_workers(player2);
  	  break;
  	}
        }
      player1->reputation = MIN(player1->reputation + GAME_REPUTATION_INCR,
				GAME_MAX_REPUTATION);
    } players_iterate_end;
  } players_iterate_end;
}

/**************************************************************************
  Send packet which tells clients that the server is starting its
  "end year" calculations (and will be sending end-turn updates etc).
  (This is referred to as "before new year" in packet and client code.)
**************************************************************************/
static void before_end_year(void)
{
  lsend_packet_generic_empty(&game.game_connections, PACKET_BEFORE_NEW_YEAR);
}

/**************************************************************************
...
**************************************************************************/
static void ai_start_turn(void)
{
  shuffled_players_iterate(pplayer) {
    if (pplayer->ai.control) {
      ai_do_first_activities(pplayer);
      flush_packets();			/* AIs can be such spammers... */
    }
  } shuffled_players_iterate_end;
}

/**************************************************************************
Handle the beginning of each turn.
Note: This does not give "time" to any player;
      it is solely for updating turn-dependent data.
**************************************************************************/
static void begin_turn(void)
{
  /* See if the value of fog of war has changed */
  if (game.fogofwar != game.fogofwar_old) {
    if (game.fogofwar) {
      enable_fog_of_war();
      game.fogofwar_old = TRUE;
    } else {
      disable_fog_of_war();
      game.fogofwar_old = FALSE;
    }
  }

  conn_list_do_buffer(&game.game_connections);

  players_iterate(pplayer) {
    freelog(LOG_DEBUG, "beginning player turn for #%d (%s)",
	    pplayer->player_no, pplayer->name);
    begin_player_turn(pplayer);
    /* human players also need this for building advice */
    ai_data_turn_init(pplayer);
  } players_iterate_end;

  players_iterate(pplayer) {
    send_player_cities(pplayer);
  } players_iterate_end;

  flush_packets();  /* to curb major city spam */
  conn_list_do_unbuffer(&game.game_connections);

  /* Try to avoid hiding events under a diplomacy dialog */
  players_iterate(pplayer) {
    if (pplayer->ai.control && !is_barbarian(pplayer)) {
      ai_diplomacy_actions(pplayer);
    }
  } players_iterate_end;
}

/**************************************************************************
...
**************************************************************************/
static void end_turn(void)
{
  nocity_send = TRUE;

  /* AI end of turn activities */
  players_iterate(pplayer) {
    if (pplayer->ai.control) {
      ai_do_last_activities(pplayer);
    }
  } players_iterate_end;

  /* Refresh cities */
  shuffled_players_iterate(pplayer) {
    great_library(pplayer);
    update_revolution(pplayer);
    player_restore_units(pplayer);
    update_city_activities(pplayer);
    pplayer->research.changed_from=-1;
    flush_packets();
  } shuffled_players_iterate_end;

  /* Unit end of turn activities */
  shuffled_players_iterate(pplayer) {
    update_unit_activities(pplayer); /* major network traffic */
    update_player_aliveness(pplayer);
    flush_packets();
    pplayer->turn_done = FALSE;
  } shuffled_players_iterate_end;

  nocity_send = FALSE;
  players_iterate(pplayer) {
    send_player_cities(pplayer);
    ai_data_turn_done(pplayer);
  } players_iterate_end;
  flush_packets();  /* to curb major city spam */

  update_environmental_upset(S_POLLUTION, &game.heating,
			     &game.globalwarming, &game.warminglevel,
			     global_warming);
  update_environmental_upset(S_FALLOUT, &game.cooling,
			     &game.nuclearwinter, &game.coolinglevel,
			     nuclear_winter);
  update_diplomatics();
  do_apollo_program();
  marco_polo_make_contact();
  make_history_report();
  send_player_turn_notifications(NULL);
  freelog(LOG_DEBUG, "Turn ended.");
  game.turn_start = time(NULL);
}

/**************************************************************************
Unconditionally save the game, with specified filename.
Always prints a message: either save ok, or failed.

Note that if !HAVE_LIBZ, then game.save_compress_level should never
become non-zero, so no need to check HAVE_LIBZ explicitly here as well.
**************************************************************************/
void save_game(char *orig_filename)
{
  char filename[600];
  struct section_file file;
  struct timer *timer_cpu, *timer_user;

  if (orig_filename && orig_filename[0] != '\0'){
    sz_strlcpy(filename, orig_filename);
  } else { /* If orig_filename is NULL or empty, use "civgame<year>m.sav" */
    my_snprintf(filename, sizeof(filename),
		"%s%+05dm.sav", game.save_name, game.year);
  }
  
  timer_cpu = new_timer_start(TIMER_CPU, TIMER_ACTIVE);
  timer_user = new_timer_start(TIMER_USER, TIMER_ACTIVE);
    
  section_file_init(&file);
  game_save(&file);

  if (game.save_compress_level > 0) {
    /* Append ".gz" to filename if not there: */
    size_t len = strlen(filename);
    if (len < 3 || strcmp(filename+len-3, ".gz") != 0) {
      sz_strlcat(filename, ".gz");
    }
  }

  if(!section_file_save(&file, filename, game.save_compress_level))
    con_write(C_FAIL, _("Failed saving game as %s"), filename);
  else
    con_write(C_OK, _("Game saved as %s"), filename);

  section_file_free(&file);

  freelog(LOG_VERBOSE, "Save time: %g seconds (%g apparent)",
	  read_timer_seconds_free(timer_cpu),
	  read_timer_seconds_free(timer_user));
}

/**************************************************************************
Save game with autosave filename, and call gamelog_save().
**************************************************************************/
static void save_game_auto(void)
{
  char filename[512];

  assert(strlen(game.save_name)<256);
  
  my_snprintf(filename, sizeof(filename),
	      "%s%+05d.sav", game.save_name, game.year);
  save_game(filename);
  gamelog_save();		/* should this be in save_game()? --dwp */
}

/**************************************************************************
...
**************************************************************************/
void start_game(void)
{
  if(server_state!=PRE_GAME_STATE) {
    con_puts(C_SYNTAX, _("The game is already running."));
    return;
  }

  con_puts(C_OK, _("Starting game."));

  server_state=SELECT_RACES_STATE; /* loaded ??? */
  force_end_of_sniff = TRUE;
}

/**************************************************************************
 Quit the server and exit.
**************************************************************************/
void server_quit(void)
{
  server_game_free();
  close_connections_and_socket();
  exit(EXIT_SUCCESS);
}

/**************************************************************************
...
**************************************************************************/
static void handle_report_request(struct connection *pconn,
				  enum report_type type)
{
  struct conn_list *dest = &pconn->self;
  
  if (server_state != RUN_GAME_STATE && server_state != GAME_OVER_STATE
      && type != REPORT_SERVER_OPTIONS1 && type != REPORT_SERVER_OPTIONS2) {
    freelog(LOG_ERROR, "Got a report request %d before game start", type);
    return;
  }

  switch(type) {
   case REPORT_WONDERS_OF_THE_WORLD:
    report_wonders_of_the_world(dest);
    break;
   case REPORT_TOP_5_CITIES:
    report_top_five_cities(dest);
    break;
   case REPORT_DEMOGRAPHIC:
    report_demographics(pconn);
    break;
  case REPORT_SERVER_OPTIONS1:
    report_server_options(dest, 1);
    break;
  case REPORT_SERVER_OPTIONS2:
    report_server_options(dest, 2);
    break;
  case REPORT_SERVER_OPTIONS: /* obsolete */
  default:
    notify_conn(dest, _("Game: request for unknown report (type %d)"), type);
  }
}

/**************************************************************************
...
**************************************************************************/
void dealloc_id(int id)
{
  used_ids[id/8]&= 0xff ^ (1<<(id%8));
}

/**************************************************************************
...
**************************************************************************/
static bool is_id_allocated(int id)
{
  return TEST_BIT(used_ids[id / 8], id % 8);
}

/**************************************************************************
...
**************************************************************************/
void alloc_id(int id)
{
  used_ids[id/8]|= (1<<(id%8));
}

/**************************************************************************
...
**************************************************************************/

int get_next_id_number(void)
{
  while (is_id_allocated(++global_id_counter) || global_id_counter == 0) {
    /* nothing */
  }
  return global_id_counter;
}

/**************************************************************************
Returns 0 if connection should be closed (because the clients was
rejected). Returns 1 else.
**************************************************************************/
bool handle_packet_input(struct connection *pconn, void *packet, int type)
{
  struct player *pplayer;

  /* a NULL packet can be returned from receive_packet_goto_route() */
  if (!packet)
    return TRUE;

  if (type == PACKET_LOGIN_REQUEST) {
    bool result = handle_login_request(pconn,
                                       (struct packet_login_request *) packet);
    free(packet);
    return result;
  }

  /* we simply ignore the packet if authentication is not enabled */
#ifdef AUTHENTICATION_ENABLED
  if (type == PACKET_AUTHENTICATION_REPLY) {
    bool result = handle_authentication_reply(pconn,
                                (struct packet_authentication_reply *) packet);
    free(packet);
    return result;
  } 
#endif

  if (type == PACKET_CONN_PONG) {
    handle_conn_pong(pconn);
    free(packet);
    return TRUE;
  }

  if (!pconn->established) {
    freelog(LOG_ERROR, "Received game packet from unaccepted connection %s",
	    conn_description(pconn));
    free(packet);
    return TRUE;
  }
  
  /* valid packets from established connections but non-players */
  if (type == PACKET_CHAT_MSG) {
    handle_chat_msg(pconn, (struct packet_generic_message *)packet);
    free(packet);
    return TRUE;
  }

  pplayer = pconn->player;

  if(!pplayer) {
    /* don't support these yet */
    freelog(LOG_ERROR, "Received packet from non-player connection %s",
 	    conn_description(pconn));
    free(packet);
    return TRUE;
  }

  if (server_state != RUN_GAME_STATE
      && type != PACKET_ALLOC_NATION
      && type != PACKET_CONN_PONG
      && type != PACKET_REPORT_REQUEST) {
    if (server_state == GAME_OVER_STATE) {
      /* This can happen by accident, so we don't want to print
	 out lots of error messages. Ie, we use LOG_DEBUG. */
      freelog(LOG_DEBUG, "got a packet of type %d "
			  "in GAME_OVER_STATE", type);
    } else {
      freelog(LOG_ERROR, "got a packet of type %d "
	                 "outside RUN_GAME_STATE", type);
    }
    free(packet);
    return TRUE;
  }

  pplayer->nturns_idle=0;

  if((!pplayer->is_alive || pconn->observer)
     && !(type == PACKET_REPORT_REQUEST || type == PACKET_CONN_PONG)) {
    freelog(LOG_ERROR, _("Got a packet of type %d from a "
			 "dead or observer player"), type);
    free(packet);
    return TRUE;
  }
  
  /* Make sure to set this back to NULL before leaving this function: */
  pplayer->current_conn = pconn;
  
  switch(type) {
    
  case PACKET_TURN_DONE:
    handle_turn_done(pplayer);
    break;

  case PACKET_ALLOC_NATION:
    handle_alloc_nation(pplayer, (struct packet_alloc_nation *)packet);
    break;

  case PACKET_UNIT_INFO:
    handle_unit_info(pplayer, (struct packet_unit_info *)packet);
    break;

  case PACKET_MOVE_UNIT:
    handle_move_unit(pplayer, (struct packet_move_unit *)packet);
    break;
 
  case PACKET_CITY_SELL:
    handle_city_sell(pplayer, (struct packet_city_request *)packet);
    break;

  case PACKET_CITY_BUY:
    handle_city_buy(pplayer, (struct packet_city_request *)packet);
    break;
   
  case PACKET_CITY_CHANGE:
    handle_city_change(pplayer, (struct packet_city_request *)packet);
    break;

  case PACKET_CITY_WORKLIST:
    handle_city_worklist(pplayer, (struct packet_city_request *)packet);
    break;

  case PACKET_CITY_MAKE_SPECIALIST:
    handle_city_make_specialist(pplayer, (struct packet_city_request *)packet);
    break;

  case PACKET_CITY_MAKE_WORKER:
    handle_city_make_worker(pplayer, (struct packet_city_request *)packet);
    break;

  case PACKET_CITY_CHANGE_SPECIALIST:
    handle_city_change_specialist(pplayer, (struct packet_city_request *)packet);
    break;

  case PACKET_CITY_RENAME:
    handle_city_rename(pplayer, (struct packet_city_request *)packet);
    break;

  case PACKET_PLAYER_RATES:
    handle_player_rates(pplayer, (struct packet_player_request *)packet);
    break;

  case PACKET_PLAYER_REVOLUTION:
    handle_player_revolution(pplayer);
    break;

  case PACKET_PLAYER_GOVERNMENT:
    handle_player_government(pplayer, (struct packet_player_request *)packet);
    break;

  case PACKET_PLAYER_RESEARCH:
    handle_player_research(pplayer, (struct packet_player_request *)packet);
    break;

  case PACKET_PLAYER_TECH_GOAL:
    handle_player_tech_goal(pplayer, (struct packet_player_request *)packet);
    break;

  case PACKET_UNIT_BUILD_CITY:
    handle_unit_build_city(pplayer, (struct packet_unit_request *)packet);
    break;

  case PACKET_UNIT_DISBAND:
    handle_unit_disband(pplayer, (struct packet_unit_request *)packet);
    break;

  case PACKET_UNIT_CHANGE_HOMECITY:
    handle_unit_change_homecity(pplayer, (struct packet_unit_request *)packet);
    break;

  case PACKET_UNIT_AUTO:
    handle_unit_auto_request(pplayer, (struct packet_unit_request *)packet);
    break;

  case PACKET_UNIT_UNLOAD:
    handle_unit_unload_request(pplayer, (struct packet_unit_request *)packet);
    break;

  case PACKET_UNITTYPE_UPGRADE:
    handle_upgrade_unittype_request(pplayer, (struct packet_unittype_info *)packet);
    break;

  case PACKET_UNIT_ESTABLISH_TRADE:
    (void) handle_unit_establish_trade(pplayer, (struct packet_unit_request *)packet);
    break;

  case PACKET_UNIT_HELP_BUILD_WONDER:
    handle_unit_help_build_wonder(pplayer, (struct packet_unit_request *)packet);
    break;

  case PACKET_UNIT_GOTO_TILE:
    handle_unit_goto_tile(pplayer, (struct packet_unit_request *)packet);
    break;
    
  case PACKET_DIPLOMAT_ACTION:
    handle_diplomat_action(pplayer, (struct packet_diplomat_action *)packet);
    break;
  case PACKET_REPORT_REQUEST:
    handle_report_request(pconn,
			  ((struct packet_generic_integer *)packet)->value);
    break;
  case PACKET_DIPLOMACY_INIT_MEETING:
    handle_diplomacy_init(pplayer, (struct packet_diplomacy_info *)packet);
    break;
  case PACKET_DIPLOMACY_CANCEL_MEETING:
    handle_diplomacy_cancel_meeting(pplayer, (struct packet_diplomacy_info *)packet);  
    break;
  case PACKET_DIPLOMACY_CREATE_CLAUSE:
    handle_diplomacy_create_clause(pplayer, (struct packet_diplomacy_info *)packet);  
    break;
  case PACKET_DIPLOMACY_REMOVE_CLAUSE:
    handle_diplomacy_remove_clause(pplayer, (struct packet_diplomacy_info *)packet);  
    break;
  case PACKET_DIPLOMACY_ACCEPT_TREATY:
    handle_diplomacy_accept_treaty(pplayer, (struct packet_diplomacy_info *)packet);  
    break;
  case PACKET_CITY_REFRESH:
    handle_city_refresh(pplayer, (struct packet_generic_integer *)packet);
    break;
  case PACKET_INCITE_INQ:
    handle_incite_inq(pconn, (struct packet_generic_integer *)packet);
    break;
  case PACKET_UNIT_UPGRADE:
    handle_unit_upgrade_request(pplayer, (struct packet_unit_request *)packet);
    break;
  case PACKET_CITY_OPTIONS:
    handle_city_options(pplayer, (struct packet_generic_values *)packet);
    break;
  case PACKET_SPACESHIP_ACTION:
    handle_spaceship_action(pplayer, (struct packet_spaceship_action *)packet);
    break;
  case PACKET_UNIT_NUKE:
    handle_unit_nuke(pplayer, (struct packet_unit_request *)packet);
    break;
  case PACKET_CITY_NAME_SUGGEST_REQ:
    handle_city_name_suggest_req(pconn,
				 (struct packet_generic_integer *)packet);
    break;
  case PACKET_UNIT_PARADROP_TO:
    handle_unit_paradrop_to(pplayer, (struct packet_unit_request *)packet);
    break;
  case PACKET_PLAYER_CANCEL_PACT:
    handle_player_cancel_pact(pplayer, (struct packet_generic_values *)packet);
    break;
  case PACKET_UNIT_CONNECT:
    handle_unit_connect(pplayer, (struct packet_unit_connect *)packet);
    break;
  case PACKET_GOTO_ROUTE:
    handle_goto_route(pplayer, (struct packet_goto_route *)packet);
    break;
  case PACKET_PATROL_ROUTE:
    handle_patrol_route(pplayer, (struct packet_goto_route *)packet);
    break;
  case PACKET_UNIT_AIRLIFT:
    handle_unit_airlift(pplayer, (struct packet_unit_request *)packet);
    break;
  case PACKET_ATTRIBUTE_CHUNK:
    handle_player_attribute_chunk(pplayer,
				  (struct packet_attribute_chunk *)
				  packet);
    break;
  case PACKET_PLAYER_ATTRIBUTE_BLOCK:
    handle_player_attribute_block(pplayer);
    break;

  default:
    freelog(LOG_ERROR, "Received unknown packet %d from %s",
	    type, conn_description(pconn));
  }

  if (pplayer->is_alive && pplayer->is_dying) {
    kill_player(pplayer);
  }
  free(packet);
  pplayer->current_conn = NULL;
  return TRUE;
}

/**************************************************************************
...
**************************************************************************/
void check_for_full_turn_done(void)
{
  /* fixedlength is only applicable if we have a timeout set */
  if (game.fixedlength && game.timeout != 0)
    return;

  players_iterate(pplayer) {
    if (game.turnblock) {
      if (!pplayer->ai.control && pplayer->is_alive && !pplayer->turn_done)
        return;
    } else {
      if(pplayer->is_connected && pplayer->is_alive && !pplayer->turn_done) {
        return;
      }
    }
  } players_iterate_end;

  force_end_of_sniff = TRUE;
}

/**************************************************************************
...
(Hmm, how should "turn done" work for multi-connected non-observer players?)
**************************************************************************/
static void handle_turn_done(struct player *pplayer)
{
  pplayer->turn_done = TRUE;

  check_for_full_turn_done();

  send_player_info(pplayer, NULL);
}

/**************************************************************************
...
**************************************************************************/
static void handle_alloc_nation(struct player *pplayer,
				struct packet_alloc_nation *packet)
{
  int nation_used_count;

  if (server_state != SELECT_RACES_STATE) {
    freelog(LOG_ERROR, _("Trying to alloc nation outside "
			 "of SELECT_RACES_STATE!"));
    return;
  }  
  
  if (packet->nation_no < 0 || packet->nation_no >= game.nation_count) {
    return;
  }

  remove_leading_trailing_spaces(packet->name);

  if (strlen(packet->name)==0) {
    notify_player(pplayer, _("Please choose a non-blank name."));
    send_select_nation(pplayer);
    return;
  }

  packet->name[0] = my_toupper(packet->name[0]);

  players_iterate(other_player) {
    if(other_player->nation==packet->nation_no) {
       send_select_nation(pplayer); /* it failed - nation taken */
       return;
    } else
      /* Check to see if name has been taken.
       * Ignore case because matches elsewhere are case-insenstive.
       * Don't limit this check to just players with allocated nation:
       * otherwise could end up with same name as pre-created AI player
       * (which have no nation yet, but will keep current player name).
       * Also want to keep all player names strictly distinct at all
       * times (for server commands etc), including during nation
       * allocation phase.
       */
      if (other_player->player_no != pplayer->player_no
	  && mystrcasecmp(other_player->name,packet->name) == 0) {
	notify_player(pplayer,
		     _("Another player already has the name '%s'.  "
		       "Please choose another name."), packet->name);
       send_select_nation(pplayer);
       return;
    }
  } players_iterate_end;

  notify_conn_ex(&game.game_connections, -1, -1, E_NATION_SELECTED,
		 _("Game: %s is the %s ruler %s."), pplayer->username,
		 get_nation_name(packet->nation_no), packet->name);

  /* inform player his choice was ok */
  lsend_packet_generic_empty(&pplayer->connections,
			     PACKET_SELECT_NATION_OK);

  pplayer->nation=packet->nation_no;
  sz_strlcpy(pplayer->name, packet->name);
  pplayer->is_male=packet->is_male;
  pplayer->city_style=packet->city_style;

  /* tell the other players, that the nation is now unavailable */
  nation_used_count = 0;

  players_iterate(other_player) {
    if (other_player->nation == NO_NATION_SELECTED) {
      send_select_nation(other_player);
    } else {
      nation_used_count++;	/* count used nations */
    }
  } players_iterate_end;

  mark_nation_as_used(packet->nation_no);

  /* if there's no nation left, reject remaining players, sorry */
  if( nation_used_count == game.playable_nation_count ) {   /* barb */
    players_iterate(other_player) {
      if (other_player->nation == NO_NATION_SELECTED) {
	freelog(LOG_NORMAL, _("No nations left: Removing player %s."),
		other_player->name);
	notify_player(other_player,
		      _("Game: Sorry, there are no nations left."));
	server_remove_player(other_player);
      }
    } players_iterate_end;
  }
}

/**************************************************************************
 Sends the currently collected selected nations to the given player.
**************************************************************************/
static void send_select_nation(struct player *pplayer)
{
  struct packet_nations_used packet;

  packet.num_nations_used = 0;

  players_iterate(other_player) {
    if (other_player->nation == NO_NATION_SELECTED) {
      continue;
    }
    packet.nations_used[packet.num_nations_used] = other_player->nation;
    packet.num_nations_used++;
  } players_iterate_end;

  lsend_packet_nations_used(&pplayer->connections, &packet);
}

/**************************************************************************
  If all players have chosen the same nation class, return
  this class, otherwise return NULL.
**************************************************************************/  
static char* find_common_class(void) 
{
  char* class = NULL;
  struct nation_type* nation;

  players_iterate(pplayer) {
    if (pplayer->nation == NO_NATION_SELECTED) {
      /* still undecided */
      continue;  
    }
    nation = get_nation_by_idx(pplayer->nation);
    assert(nation->class != NULL);
    if (class == NULL) {
       /* Set the class. */
      class = nation->class;
    } else if (strcmp(nation->class, class) != 0) {
      /* Multiple classes are already being used. */
      return NULL;
    }
  } players_iterate_end;

  return class;
}

/**************************************************************************
  Select a random available nation.  If 'class' is non-NULL, then choose
  a nation from that class if possible.
**************************************************************************/
static Nation_Type_id select_random_nation(const char* class)
{
  Nation_Type_id* nations, selected;
  int i, j;
  
  if (class == NULL) {
    return nations_avail[myrand(num_nations_avail)];
  }
  
  nations = fc_malloc(num_nations_avail * sizeof(*nations));
  for (j = i = 0; i < num_nations_avail; i++) {
    struct nation_type* nation = get_nation_by_idx(nations_avail[i]);

    assert(nation->class != NULL);
    if (strcmp(nation->class, class) == 0) {
      nations[j++] = nations_avail[i];
    }
  }

  if (j == 0) {
    /* Pick any available nation. */
    selected = nations_avail[myrand(num_nations_avail)];
  } else {
    selected = nations[myrand(j)];
    assert(strcmp(get_nation_by_idx(selected)->class, class) == 0);
  }

  free(nations);
  return selected;
}

/**************************************************************************
generate_ai_players() - Selects a nation for players created with
   server's "create <PlayerName>" command.  If <PlayerName> matches
   one of the leader names for some nation, we choose that nation.
   (I.e. if we issue "create Shaka" then we will make that AI player's
   nation the Zulus if the Zulus have not been chosen by anyone else.
   If they have, then we pick an available nation at random.)

   After that, we check to see if the server option "aifill" is greater
   than the number of players currently connected.  If so, we create the
   appropriate number of players (game.aifill - game.nplayers) from
   scratch, choosing a random nation and appropriate name for each.
   
   When we choose a nation randomly we try to consider only nations
   that are in the same class as nations choosen by other players.
   (I.e., if human player decides to play English, AI won't use Mordorians.)

   If the AI player name is one of the leader names for the AI player's
   nation, the player sex is set to the sex for that leader, else it
   is chosen randomly.  (So if English are ruled by Elisabeth, she is
   female, but if "Player 1" rules English, may be male or female.)
**************************************************************************/
static void generate_ai_players(void)
{
  Nation_Type_id nation;
  char player_name[MAX_LEN_NAME];
  struct player *pplayer;
  int i, old_nplayers;
  char* common_class;

  /* Select nations for AI players generated with server
   * 'create <name>' command
   */
  common_class = find_common_class();
  for (i=0; i<game.nplayers; i++) {
    pplayer = &game.players[i];
    
    if (pplayer->nation != NO_NATION_SELECTED) {
      continue;
    }

    if (num_nations_avail == 0) {
      freelog(LOG_NORMAL,
	      _("Ran out of nations.  AI controlled player %s not created."),
	      pplayer->name );
      server_remove_player(pplayer); 
      /*
       * Below decrement loop index 'i' so that the loop is redone with
       * the current index (if 'i' is still less than new game.nplayers).
       * This is because subsequent players in list will have been shifted
       * down one spot by the remove, and may need handling.
       */
      i--;  
      continue;
    }

    for (nation = 0; nation < game.playable_nation_count; nation++) {
      if (check_nation_leader_name(nation, pplayer->name)) {
        if (nations_used[nation] != -1) {
	  pplayer->nation = mark_nation_as_used(nation);
	  pplayer->city_style = get_nation_city_style(nation);
          pplayer->is_male = get_nation_leader_sex(nation, pplayer->name);
	  break;
        }
      }
    }

    if (nation == game.playable_nation_count) {
      pplayer->nation =
	mark_nation_as_used(select_random_nation(common_class));
      pplayer->city_style = get_nation_city_style(pplayer->nation);
      pplayer->is_male = (myrand(2) == 1);
    }

    announce_ai_player(pplayer);
  }
  
  /* We do this again, because user could type:
   * >create Hammurabi
   * >set aifill 5
   * Now we are sure that all AI-players will use historical class
   */
  common_class = find_common_class();

  /* Create and pick nation and name for AI players needed to bring the
   * total number of players to equal game.aifill
   */

  if (game.playable_nation_count < game.aifill) {
    game.aifill = game.playable_nation_count;
    freelog(LOG_NORMAL,
	     _("Nation count smaller than aifill; aifill reduced to %d."),
             game.playable_nation_count);
  }

  if (game.max_players < game.aifill) {
    game.aifill = game.max_players;
    freelog(LOG_NORMAL,
	     _("Maxplayers smaller than aifill; aifill reduced to %d."),
             game.max_players);
  }

  for(;game.nplayers < game.aifill;) {
    nation = mark_nation_as_used(select_random_nation(common_class));
    pick_ai_player_name(nation, player_name);

    old_nplayers = game.nplayers;
    pplayer = get_player(old_nplayers);
     
    sz_strlcpy(pplayer->name, player_name);
    sz_strlcpy(pplayer->username, ANON_USER_NAME);

    freelog(LOG_NORMAL, _("%s has been added as an AI-controlled player."),
            player_name);
    notify_player(NULL,
                  _("Game: %s has been added as an AI-controlled player."),
                  player_name);

    game.nplayers++;

    (void) send_server_info_to_metaserver(TRUE, FALSE);

    if (!((game.nplayers == old_nplayers+1)
	  && strcmp(player_name, pplayer->name)==0)) {
      con_write(C_FAIL, _("Error creating new AI player: %s\n"),
		player_name);
      break;			/* don't loop forever */
    }
      
    pplayer->nation = nation;
    pplayer->city_style = get_nation_city_style(nation);
    pplayer->ai.control = TRUE;
    pplayer->ai.skill_level = game.skill_level;
    if (check_nation_leader_name(nation, player_name)) {
      pplayer->is_male = get_nation_leader_sex(nation, player_name);
    } else {
      pplayer->is_male = (myrand(2) == 1);
    }
    announce_ai_player(pplayer);
    set_ai_level_direct(pplayer, pplayer->ai.skill_level);
  }
  (void) send_server_info_to_metaserver(TRUE, FALSE);
}

/*************************************************************************
 Used in pick_ai_player_name() below; buf has size at least MAX_LEN_NAME;
*************************************************************************/
static bool good_name(char *ptry, char *buf) {
  if (!(find_player_by_name(ptry) || find_player_by_user(ptry))) {
     (void) mystrlcpy(buf, ptry, MAX_LEN_NAME);
     return TRUE;
  }
  return FALSE;
}

/*************************************************************************
 pick_ai_player_name() - Returns a random ruler name picked from given nation
     ruler names, given that nation's number. If that player name is already 
     taken, iterates through all leader names to find unused one. If it fails
     it iterates through "Player 1", "Player 2", ... until an unused name
     is found.
 newname should point to a buffer of size at least MAX_LEN_NAME.
*************************************************************************/
void pick_ai_player_name(Nation_Type_id nation, char *newname) 
{
   int i, names_count;
   struct leader *leaders;

   leaders = get_nation_leaders(nation, &names_count);

   /* Try random names (scattershot), then all available,
    * then "Player 1" etc:
    */
   for(i=0; i<names_count; i++) {
     if (good_name(leaders[myrand(names_count)].name, newname)) {
       return;
     }
   }
   
   for(i=0; i<names_count; i++) {
     if (good_name(leaders[i].name, newname)) {
       return;
     }
   }
   
   for(i=1; /**/; i++) {
     char tempname[50];
     my_snprintf(tempname, sizeof(tempname), _("Player %d"), i);
     if (good_name(tempname, newname)) return;
   }
}

/*************************************************************************
 mark_nation_as_used() - shuffles the appropriate arrays to indicate that
 the specified nation number has been allocated to some player and is
 therefore no longer available to any other player.  We do things this way
 so that the process of determining which nations are available to AI players
 is more efficient.
*************************************************************************/
static int mark_nation_as_used (int nation) 
{
  if (num_nations_avail <= 0) {	/* no more unused nation */
    die("Argh! ran out of nations!");
  }

   nations_used[nations_avail[num_nations_avail-1]]=nations_used[nation];
   nations_avail[nations_used[nation]]=nations_avail[--num_nations_avail];
   nations_used[nation]=-1;

   return nation;
}

/*************************************************************************
...
*************************************************************************/
static void announce_ai_player (struct player *pplayer) {
   freelog(LOG_NORMAL, _("AI is controlling the %s ruled by %s."),
                    get_nation_name_plural(pplayer->nation),
                    pplayer->name);

  players_iterate(other_player) {
    notify_player(other_player,
		  _("Game: %s rules the %s."), pplayer->name,
		  get_nation_name_plural(pplayer->nation));
  } players_iterate_end;
}

/**************************************************************************
Play the game! Returns when server_state == GAME_OVER_STATE.
**************************************************************************/
static void main_loop(void)
{
  struct timer *eot_timer;	/* time server processing at end-of-turn */
  int save_counter = 0;

  eot_timer = new_timer_start(TIMER_CPU, TIMER_ACTIVE);

  /* 
   * This will freeze the reports and agents at the client.
   * 
   * Do this before the body so that the PACKET_THAW_HINT packet is
   * balanced. 
   */
  lsend_packet_generic_empty(&game.game_connections, PACKET_FREEZE_HINT);

  while(server_state==RUN_GAME_STATE) {
    /* absolute beginning of a turn */
    freelog(LOG_DEBUG, "Begin turn");
    begin_turn();

#if (IS_DEVEL_VERSION || IS_BETA_VERSION)
    sanity_check();
#endif

    force_end_of_sniff = FALSE;

    freelog(LOG_DEBUG, "Shuffleplayers");
    shuffle_players();
    freelog(LOG_DEBUG, "Aistartturn");
    ai_start_turn();
    send_start_turn_to_clients();

    /* 
     * This will thaw the reports and agents at the client.
     */
    lsend_packet_generic_empty(&game.game_connections, PACKET_THAW_HINT);

    /* Before sniff (human player activites), report time to now: */
    freelog(LOG_VERBOSE, "End/start-turn server/ai activities: %g seconds",
	    read_timer_seconds(eot_timer));

    /* Do auto-saves just before starting sniff_packets(), so that
     * autosave happens effectively "at the same time" as manual
     * saves, from the point of view of restarting and AI players.
     * Post-increment so we don't count the first loop.
     */
    if(save_counter >= game.save_nturns && game.save_nturns>0) {
      save_counter=0;
      save_game_auto();
    }
    save_counter++;
    
    freelog(LOG_DEBUG, "sniffingpackets");
    while (sniff_packets() == 1) {
      /* nothing */
    }

    /* After sniff, re-zero the timer: (read-out above on next loop) */
    clear_timer_start(eot_timer);
    
    conn_list_do_buffer(&game.game_connections);

#if (IS_DEVEL_VERSION || IS_BETA_VERSION)
    sanity_check();
#endif

    /* 
     * This empties the client Messages window; put this before
     * everything else below, since otherwise any messages from the
     * following parts get wiped out before the user gets a chance to
     * see them.  --dwp
    */
    before_end_year();

    /* 
     * This will freeze the reports and agents at the client.
     */
    lsend_packet_generic_empty(&game.game_connections, PACKET_FREEZE_HINT);

    freelog(LOG_DEBUG, "Season of native unrests");
    summon_barbarians(); /* wild guess really, no idea where to put it, but
                            I want to give them chance to move their units */
    /* Moved this to after the human turn for efficiency -- Syela */
    freelog(LOG_DEBUG, "Autosettlers");
    auto_settlers();
    freelog(LOG_DEBUG, "Auto-Attack phase");
    auto_attack();
    freelog(LOG_DEBUG, "Endturn");
    end_turn();
    freelog(LOG_DEBUG, "Gamenextyear");
    game_advance_year();
    freelog(LOG_DEBUG, "Updatetimeout");
    update_timeout();
    check_spaceship_arrivals();
    freelog(LOG_DEBUG, "Sendplayerinfo");
    send_player_info(NULL, NULL);
    freelog(LOG_DEBUG, "Sendgameinfo");
    send_game_info(NULL);
    freelog(LOG_DEBUG, "Sendyeartoclients");
    send_year_to_clients(game.year);
    freelog(LOG_DEBUG, "Sendinfotometaserver");
    (void) send_server_info_to_metaserver(FALSE, FALSE);

    conn_list_do_unbuffer(&game.game_connections);

    if (is_game_over()) {
      server_state=GAME_OVER_STATE;
    }
  }

  /* 
   * This will thaw the reports and agents at the client.
   */
  lsend_packet_generic_empty(&game.game_connections, PACKET_THAW_HINT);
}

/**************************************************************************
  Server initialization.
**************************************************************************/
void srv_main(void)
{
  /* make sure it's initialized */
  if (!has_been_srv_init) {
    srv_init();
  }

  my_init_network();

  con_log_init(srvarg.log_filename, srvarg.loglevel);
  gamelog_init(srvarg.gamelog_filename);
  gamelog_set_level(GAMELOG_FULL);
  gamelog(GAMELOG_NORMAL, _("Starting new log"));
  
#if IS_BETA_VERSION
  con_puts(C_COMMENT, "");
  con_puts(C_COMMENT, beta_message());
  con_puts(C_COMMENT, "");
#endif
  
  con_flush();

  game_init();

  /* init network */  
  init_connections(); 
  server_open_socket();

  /* load a saved game */
  if (srvarg.load_filename) {
    load_command(NULL, srvarg.load_filename);
  } 

  if(!(srvarg.metaserver_no_send)) {
    freelog(LOG_NORMAL, _("Sending info to metaserver [%s]"),
	    meta_addr_port());
    server_open_udp(); /* open socket for meta server */ 
  }

  (void) send_server_info_to_metaserver(TRUE, FALSE);

  /* accept new players, wait for serverop to start..*/
  server_state = PRE_GAME_STATE;

  /* load a script file */
  if (srvarg.script_filename
      && !read_init_script(NULL, srvarg.script_filename)) {
    exit(EXIT_FAILURE);
  }

  /* Run server loop */
  while (TRUE) {
    cm_init(); /* initialize CM */
    srv_loop();
    if (game.timeout == -1) {
      server_quit();
    }

    send_game_state(&game.game_connections, CLIENT_GAME_OVER_STATE);
    report_final_scores();
    show_map_to_all();
    notify_player(NULL, _("Game: The game is over..."));
    gamelog(GAMELOG_NORMAL, _("The game is over!"));
    if (game.save_nturns > 0) {
      save_game_auto();
    }

    /* Remain in GAME_OVER_STATE until players log out */
    while (conn_list_size(&game.est_connections) > 0) {
      (void) sniff_packets();
    }

    /* Reset server */
    server_game_free();
    game_init();
    game.is_new_game = TRUE;
    server_state = PRE_GAME_STATE;
  }

  /* Technically, we won't ever get here. We exit via server_quit. */
}

/**************************************************************************
  Server loop, run to set up one game.
**************************************************************************/
static void srv_loop(void)
{
  int i;

  freelog(LOG_NORMAL, _("Now accepting new client connections."));
  while(server_state == PRE_GAME_STATE) {
    sniff_packets(); /* Accepting commands. */
  }

  (void) send_server_info_to_metaserver(TRUE, FALSE);

  if (game.is_new_game) {
    load_rulesets();
    /* otherwise rulesets were loaded when savegame was loaded */
  }

  nations_avail = fc_calloc(game.playable_nation_count, sizeof(int));
  nations_used = fc_calloc(game.playable_nation_count, sizeof(int));

main_start_players:

  send_rulesets(&game.game_connections);

  num_nations_avail = game.playable_nation_count;
  for (i = 0; i < game.playable_nation_count; i++) {
    nations_avail[i] = i;
    nations_used[i] = i;
  }

  if (game.auto_ai_toggle) {
    players_iterate(pplayer) {
      if (!pplayer->is_connected && !pplayer->ai.control) {
	toggle_ai_player_direct(NULL, pplayer);
      }
    } players_iterate_end;
  }

  /* Allow players to select a nation (case new game).
   * AI players may not yet have a nation; these will be selected
   * in generate_ai_players() later
   */
  server_state = RUN_GAME_STATE;
  players_iterate(pplayer) {
    if (pplayer->nation == NO_NATION_SELECTED && !pplayer->ai.control) {
      send_select_nation(pplayer);
      server_state = SELECT_RACES_STATE;
    }
  } players_iterate_end;

  while(server_state == SELECT_RACES_STATE) {
    bool flag = FALSE;

    sniff_packets();

    players_iterate(pplayer) {
      if (pplayer->nation == NO_NATION_SELECTED && !pplayer->ai.control) {
	flag = TRUE;
	break;
      }
    } players_iterate_end;

    if (!flag) {
      if (game.nplayers > 0) {
	server_state = RUN_GAME_STATE;
      } else {
	con_write(C_COMMENT,
		  _("Last player has disconnected: will need to restart."));
	server_state = PRE_GAME_STATE;
	while(server_state == PRE_GAME_STATE) {
	  sniff_packets();
	}
	goto main_start_players;
      }
    }
  }

  if (game.randseed == 0) {
    /* We strip the high bit for now because neither game file nor
       server options can handle unsigned ints yet. - Cedric */
    game.randseed = time(NULL) & (MAX_UINT32 >> 1);
  }
 
  if (!myrand_is_init()) {
    mysrand(game.randseed);
  }

#ifdef TEST_RANDOM /* not defined anywhere, set it if you want it */
  test_random1(200);
  test_random1(2000);
  test_random1(20000);
  test_random1(200000);
#endif
    
  if (game.is_new_game) {
    generate_ai_players();
  }
   
  /* if we have a tile map, and map.generator==0, call map_fractal_generate
     anyway, to make the specials and huts */
  if (map_is_empty() || (map.generator == 0 && game.is_new_game)) {
    map_fractal_generate();
  }

  if (map.num_continents == 0) {
    assign_continent_numbers();
  }

  gamelog_map();
  /* start the game */

  server_state = RUN_GAME_STATE;
  (void) send_server_info_to_metaserver(TRUE, FALSE);

  if(game.is_new_game) {
    /* Before the player map is allocated (and initiailized)! */
    game.fogofwar_old = game.fogofwar;

    allot_island_improvs();

    players_iterate(pplayer) {
      player_map_allocate(pplayer);
      init_tech(pplayer, game.tech);
      player_limit_to_government_rates(pplayer);
      pplayer->economic.gold = game.gold;
    } players_iterate_end;
    game.max_players = game.nplayers;

    /* we don't want random start positions in a scenario which already
       provides them. -- Gudy */
    if(map.num_start_positions == 0) {
      create_start_positions();
    }
  }

  /* Set up alliances based on team selections */
  if (game.is_new_game) {
   players_iterate(pplayer) {
     players_iterate(pdest) {
      if (pplayer->team == pdest->team && pplayer->team != TEAM_NONE
          && pplayer->player_no != pdest->player_no) {
        pplayer->diplstates[pdest->player_no].type = DS_ALLIANCE;
        give_shared_vision(pplayer, pdest);
        pplayer->embassy |= (1 << pdest->player_no);
      }
    } players_iterate_end;
   } players_iterate_end;
  }

  initialize_move_costs(); /* this may be the wrong place to do this */
  generate_minimap(); /* for city_desire; saves a lot of calculations */

  if (!game.is_new_game) {
    players_iterate(pplayer) {
      civ_score(pplayer);	/* if we don't, the AI gets really confused */
      if (pplayer->ai.control) {
	set_ai_level_direct(pplayer, pplayer->ai.skill_level);
      }
    } players_iterate_end;
  }

  players_iterate(pplayer) {
    ai_data_init(pplayer); /* Initialize this at last moment */
  } players_iterate_end;
  
  /* We want to reset the timer as late as possible but before the info is
   * sent to the clients */
  game.turn_start = time(NULL);

  lsend_packet_generic_empty(&game.game_connections, PACKET_FREEZE_HINT);
  send_all_info(&game.game_connections);
  lsend_packet_generic_empty(&game.game_connections, PACKET_THAW_HINT);
  
  if(game.is_new_game) {
    init_new_game();
  }

  game.is_new_game = FALSE;

  send_game_state(&game.game_connections, CLIENT_GAME_RUNNING_STATE);

  /*** Where the action is. ***/
  main_loop();
}

/**************************************************************************
 ...
**************************************************************************/
void server_game_free()
{
  cm_free();
  players_iterate(pplayer) {
    player_map_free(pplayer);
  } players_iterate_end;

  nation_city_names_free(misc_city_names);
  misc_city_names = NULL;
  game_free();
}
