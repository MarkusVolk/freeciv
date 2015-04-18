/****************************************************************************
 Freeciv - Copyright (C) 2004 - The Freeciv Team
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
****************************************************************************/

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

/* utility */
#include "log.h"
#include "rand.h"

/* common */
#include "city.h"
#include "effects.h"
#include "game.h"
#include "map.h"
#include "tile.h"

#include "traderoutes.h"

const char *trade_route_type_names[] = {
  "National", "NationalIC", "IN", "INIC", "Ally", "AllyIC",
  "Enemy", "EnemyIC", "Team", "TeamIC"
};

const char *traderoute_cancelling_type_names[] = {
  "Active", "Inactive", "Cancel"
};

struct trade_route_settings trtss[TRT_LAST];

static struct goods_type goods[MAX_GOODS_TYPES];

/*************************************************************************
  Return current maximum number of trade routes city can have.
*************************************************************************/
int max_trade_routes(const struct city *pcity)
{
  int eft = get_city_bonus(pcity, EFT_MAX_TRADE_ROUTES);

  return CLIP(0, eft, MAX_TRADE_ROUTES);
}

/*************************************************************************
  What is type of the traderoute between two cities.
*************************************************************************/
enum trade_route_type cities_trade_route_type(const struct city *pcity1,
                                              const struct city *pcity2)
{
  struct player *plr1 = city_owner(pcity1);
  struct player *plr2 = city_owner(pcity2);

  if (plr1 != plr2) {
    struct player_diplstate *ds = player_diplstate_get(plr1, plr2);

    if (city_tile(pcity1)->continent != city_tile(pcity2)->continent) {
      switch (ds->type) {
      case DS_ALLIANCE:
        return TRT_ALLY_IC;
      case DS_WAR:
        return TRT_ENEMY_IC;
      case DS_TEAM:
        return TRT_TEAM_IC;
      case DS_ARMISTICE:
      case DS_CEASEFIRE:
      case DS_PEACE:
      case DS_NO_CONTACT:
        return TRT_IN_IC;
      case DS_LAST:
        fc_assert(ds->type != DS_LAST);
        return TRT_IN_IC;
      }
      fc_assert(FALSE);

      return TRT_IN_IC;
    } else {
      switch (ds->type) {
      case DS_ALLIANCE:
        return TRT_ALLY;
      case DS_WAR:
        return TRT_ENEMY;
      case DS_TEAM:
        return TRT_TEAM;
      case DS_ARMISTICE:
      case DS_CEASEFIRE:
      case DS_PEACE:
      case DS_NO_CONTACT:
        return TRT_IN;
      case DS_LAST:
        fc_assert(ds->type != DS_LAST);
        return TRT_IN;
      }
      fc_assert(FALSE);

      return TRT_IN;
    }
  } else {
    if (city_tile(pcity1)->continent != city_tile(pcity2)->continent) {
      return TRT_NATIONAL_IC;
    } else {
      return TRT_NATIONAL;
    }
  }

  return TRT_LAST;
}

/*************************************************************************
  Return percentage bonus for trade route type.
*************************************************************************/
int trade_route_type_trade_pct(enum trade_route_type type)
{
  if (type < 0 || type >= TRT_LAST) {
    return 0;
  }

  return trtss[type].trade_pct;
}

/*************************************************************************
  Initialize trade route types.
*************************************************************************/
void trade_route_types_init(void)
{
  enum trade_route_type type;

  for (type = TRT_NATIONAL; type < TRT_LAST; type++) {
    struct trade_route_settings *set = trade_route_settings_by_type(type);

    set->trade_pct = 100;
  }
}

/*************************************************************************
  Return human readable name of trade route type
*************************************************************************/
const char *trade_route_type_name(enum trade_route_type type)
{
  fc_assert_ret_val(type >= TRT_NATIONAL && type < TRT_LAST, NULL);

  return trade_route_type_names[type];
}

/*************************************************************************
  Get trade route type by name.
*************************************************************************/
enum trade_route_type trade_route_type_by_name(const char *name)
{
  enum trade_route_type type;

  for (type = TRT_NATIONAL; type < TRT_LAST; type++) {
    if (!fc_strcasecmp(trade_route_type_names[type], name)) {
      return type;
    }
  }

  return TRT_LAST;
}

/*************************************************************************
  Return human readable name of traderoute cancelling type
*************************************************************************/
const char *traderoute_cancelling_type_name(enum traderoute_illegal_cancelling type)
{
  fc_assert_ret_val(type >= TRI_ACTIVE && type < TRI_LAST, NULL);

  return traderoute_cancelling_type_names[type];
}

/*************************************************************************
  Get traderoute cancelling type by name.
*************************************************************************/
enum traderoute_illegal_cancelling traderoute_cancelling_type_by_name(const char *name)
{
  enum traderoute_illegal_cancelling type;

  for (type = TRI_ACTIVE; type < TRI_LAST; type++) {
    if (!fc_strcasecmp(traderoute_cancelling_type_names[type], name)) {
      return type;
    }
  }

  return TRI_LAST;
}

/*************************************************************************
  Get trade route settings related to type.
*************************************************************************/
struct trade_route_settings *
trade_route_settings_by_type(enum trade_route_type type)
{
  fc_assert_ret_val(type >= TRT_NATIONAL && type < TRT_LAST, NULL);

  return &trtss[type];
}

/**************************************************************************
  Return TRUE iff the two cities are capable of trade; i.e., if a caravan
  from one city can enter the other to sell its goods.

  See also can_establish_trade_route().
**************************************************************************/
bool can_cities_trade(const struct city *pc1, const struct city *pc2)
{
  /* If you change the logic here, make sure to update the help in
   * helptext_unit(). */
  return (pc1 && pc2 && pc1 != pc2
          && (city_owner(pc1) != city_owner(pc2)
              || map_distance(pc1->tile, pc2->tile)
                 >= game.info.trademindist)
          && (trade_route_type_trade_pct(cities_trade_route_type(pc1, pc2))
              > 0));
}

/****************************************************************************
  Return the minimum value of the sum of trade routes which could be
  replaced by a new one. The target cities of the concerned trade routes
  are will be put into 'would_remove', if set.
****************************************************************************/
int city_trade_removable(const struct city *pcity,
                         struct city_list *would_remove)
{
  int sorted[MAX_TRADE_ROUTES];
  int num, i, j;

  FC_STATIC_ASSERT(ARRAY_SIZE(sorted) == ARRAY_SIZE(pcity->trade),
                   incompatible_trade_array_size);
  FC_STATIC_ASSERT(ARRAY_SIZE(sorted) == ARRAY_SIZE(pcity->trade_value),
                   incompatible_trade_value_array_size);

  /* Sort trade route values. */
  num = 0;
  for (i = 0; i < MAX_TRADE_ROUTES; i++) {
    if (0 == pcity->trade[i]) {
      continue;
    }
    for (j = num; j > 0 && (pcity->trade_value[i]
                            < pcity->trade_value[sorted[j - 1]]); j--) {
      sorted[j] = sorted[j - 1];
    }
    sorted[j] = i;
    num++;
  }

  /* No trade routes at all. */
  if (0 == num) {
    return 0;
  }

  /* Adjust number of concerned trade routes. */
  num += 1 - max_trade_routes(pcity);
  if (0 >= num) {
    num = 1;
  }

  /* Return values. */
  for (i = j = 0; i < num; i++) {
    j += pcity->trade_value[sorted[i]];
    if (NULL != would_remove) {
      struct city *pother = game_city_by_number(pcity->trade[sorted[i]]);

      fc_assert(NULL != pother);
      city_list_append(would_remove, pother);
    }
  }
  return j;
}

/**************************************************************************
  Returns TRUE iff the two cities can establish a trade route.  We look
  at the distance and ownership of the cities as well as their existing
  trade routes.  Should only be called if you already know that
  can_cities_trade().
**************************************************************************/
bool can_establish_trade_route(const struct city *pc1, const struct city *pc2)
{
  int trade = -1;
  int maxpc1;
  int maxpc2;

  if (!pc1 || !pc2 || pc1 == pc2
      || !can_cities_trade(pc1, pc2)
      || have_cities_trade_route(pc1, pc2)) {
    return FALSE;
  }

  /* First check if cities can have trade routes at all. */
  maxpc1 = max_trade_routes(pc1);
  if (maxpc1 <= 0) {
    return FALSE;
  }
  maxpc2 = max_trade_routes(pc2);
  if (maxpc2 <= 0) {
    return FALSE;
  }
    
  if (city_num_trade_routes(pc1) >= maxpc1) {
    trade = trade_between_cities(pc1, pc2);
    /* can we replace trade route? */
    if (city_trade_removable(pc1, NULL) >= trade) {
      return FALSE;
    }
  }
  
  if (city_num_trade_routes(pc2) >= maxpc2) {
    if (trade == -1) {
      trade = trade_between_cities(pc1, pc2);
    }
    /* can we replace trade route? */
    if (city_trade_removable(pc2, NULL) >= trade) {
      return FALSE;
    }
  }  

  return TRUE;
}

/**************************************************************************
  Return the trade that exists between these cities, assuming they have a
  trade route.
**************************************************************************/
int trade_between_cities(const struct city *pc1, const struct city *pc2)
{
  int bonus = 0;

  if (NULL != pc1 && NULL != pc1->tile
      && NULL != pc2 && NULL != pc2->tile) {

    bonus = real_map_distance(pc1->tile, pc2->tile)
            + city_size_get(pc1) + city_size_get(pc2);

    bonus = bonus * trade_route_type_trade_pct(cities_trade_route_type(pc1, pc2)) / 100;

    bonus /= 12;
  }

  return bonus;
}

/**************************************************************************
 Return number of trade route city has
**************************************************************************/
int city_num_trade_routes(const struct city *pcity)
{
  int i, n = 0;

  for (i = 0; i < MAX_TRADE_ROUTES; i++) {
    if (pcity->trade[i] != 0) {
      n++;
    }
  }
  
  return n;
}

/**************************************************************************
  Returns the revenue trade bonus - you get this when establishing a
  trade route and also when you simply sell your trade goods at the
  new city.

  If you change this calculation remember to also update its duplication
  in dai_choose_trade_route()
**************************************************************************/
int get_caravan_enter_city_trade_bonus(const struct city *pc1, 
                                       const struct city *pc2,
                                       const bool establish_trade)
{
  int tb, bonus;

  /* Should this be real_map_distance? */
  tb = map_distance(pc1->tile, pc2->tile) + 10;
  tb = (tb * (pc1->surplus[O_TRADE] + pc2->surplus[O_TRADE])) / 24;

  /*  fudge factor to more closely approximate Civ2 behavior (Civ2 is
   * really very different -- this just fakes it a little better) */
  tb *= 3;
  
  /* Trade_revenue_bonus increases revenue by power of 2 in milimes */
  bonus = get_city_bonus(pc1, EFT_TRADE_REVENUE_BONUS);
  
  tb = (float)tb * pow(2.0, (double)bonus / 1000.0);

  if (!establish_trade) {
    /* There will only be a full bonus if a new trade route is
     * established. The one time bonus from Enter Marketplace is about one
     * third of the one time bonus from Establish Trade Route. */
    tb = (tb + 2) / 3;
  }

  return tb;
}

/**************************************************************************
  Check if cities have an established trade route.
**************************************************************************/
bool have_cities_trade_route(const struct city *pc1, const struct city *pc2)
{
  trade_routes_iterate(pc1, route_to) {
    if (route_to->id == pc2->id) {
      return TRUE;
    }
  } trade_routes_iterate_end;

  return FALSE;
}

/****************************************************************************
  Initialize goods structures.
****************************************************************************/
void goods_init(void)
{
  int i;

  for (i = 0; i < MAX_GOODS_TYPES; i++) {
    goods[i].id = i;
  }
}

/****************************************************************************
  Free the memory associated with goods
****************************************************************************/
void goods_free(void)
{
}

/**************************************************************************
  Return the goods id.
**************************************************************************/
Goods_type_id goods_number(const struct goods_type *pgood)
{
  fc_assert_ret_val(NULL != pgood, -1);

  return pgood->id;
}

/**************************************************************************
  Return the goods index.

  Currently same as goods_number(), paired with goods_count()
  indicates use as an array index.
**************************************************************************/
Goods_type_id goods_index(const struct goods_type *pgood)
{
  fc_assert_ret_val(NULL != pgood, -1);

  return pgood - goods;
}

/****************************************************************************
  Return goods type of given id.
****************************************************************************/
struct goods_type *goods_by_number(Goods_type_id id)
{
  fc_assert_ret_val(id >= 0 && id < game.control.num_goods_types, NULL);

  return &goods[id];
}

/****************************************************************************
  Return translated name of this goods type.
****************************************************************************/
const char *goods_name_translation(struct goods_type *pgood)
{
  return name_translation(&pgood->name);
}

/****************************************************************************
  Return untranslated name of this goods type.
****************************************************************************/
const char *goods_rule_name(struct goods_type *pgood)
{
  return rule_name(&pgood->name);
}

/****************************************************************************
  Return goods type for the new traderoute between given cities.
****************************************************************************/
struct goods_type *goods_for_new_route(struct city *src, struct city *dest)
{
  return goods_by_number(fc_rand(game.control.num_goods_types));
}
