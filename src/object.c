/*
 * Dynamic Lua binding to GObject using dynamic gobject-introspection.
 *
 * Copyright (c) 2010, 2011 Pavel Holejsovsky
 * Licensed under the MIT license:
 * http://www.opensource.org/licenses/mit-license.php
 *
 * GObject and GTypeInstance handling.
 */

#include <string.h>
#include "lgi.h"

/* lightuserdata keys to registry, containing tables representing
   strong and weak caches.  Objects are always in weak cache, and
   added/removed from strong cache according to gobject's toggle_ref
   notifications. */
static int cache_weak, cache_strong;

/* lightuserdata key to registry for metatable of objects. */
static int object_mt;

/* Checks that given narg is object type and returns pointer to type
   instance representing it. */
static gpointer
object_check (lua_State *L, int narg)
{
  gpointer *obj = lua_touserdata (L, narg);
  luaL_checkstack (L, 3, "");
  if (!lua_getmetatable (L, narg))
    return NULL;
  lua_pushlightuserdata (L, &object_mt);
  lua_rawget (L, LUA_REGISTRYINDEX);
  if (!lua_equal (L, -1, -2))
    obj = NULL;

  lua_pop (L, 2);
  g_assert (obj == NULL || *obj != NULL);
  return obj ? *obj : NULL;
}

/* Walks given type and tries to find the closest known match of the
   object present in the repo. If found, leaves found type table on
   the stack and returns real found gtype, otherwise returns
   G_TYPE_INVALID. */
static GType
object_type (lua_State *L, GType gtype)
{
  for (; gtype != G_TYPE_INVALID; gtype = g_type_parent (gtype))
    {
      /* Get appropriate repo table, if present. */
      lgi_type_get_repotype (L, gtype, NULL);
      if (!lua_isnil (L, -1))
	break;

      lua_pop (L, 1);
    }

  return gtype;
}

/* Throws type error for object at given argument, gtype can
   optionally contain name of requested type. */
static int
object_type_error (lua_State *L, int narg, GType gtype)
{
  GType found_gtype;
  /* Look up type table and get name from it. */
  luaL_checkstack (L, 4, "");
  found_gtype = object_type (L, gtype);
  if (found_gtype != G_TYPE_INVALID)
    {
      lua_getfield (L, -1, "_name");
      lua_pushfstring (L, gtype == found_gtype ? "%s" : "%s(%s)",
		       lua_tostring (L, -1), g_type_name (gtype));
    }
  else
    {
      if (gtype == G_TYPE_INVALID)
	lua_pushliteral (L, "lgi.object");
      else
	lua_pushstring (L, g_type_name (gtype));
    }

  /* Create error message. */
  lua_pushstring (L, lua_typename (L, lua_type (L, narg)));
  lua_pushfstring (L, "%s expected, got %s", lua_tostring (L, -2),
		   lua_tostring (L, -1));
  return luaL_argerror (L, narg, lua_tostring (L, -1));
}

static gpointer
object_get (lua_State *L, int narg)
{
  gpointer obj = object_check (L, narg);
  if (G_UNLIKELY (!obj))
    object_type_error (L, narg, G_TYPE_INVALID);
  return obj;
}

/* GObject toggle-ref notification callback.  Inserts or removes given
   object from/to strong reference cache. */
static void
object_toggle_notify (gpointer data, GObject *object, gboolean is_last_ref)
{
  lua_State *L = lgi_callback_enter (data);
  luaL_checkstack (L, 3, "");
  lua_pushlightuserdata (L, &cache_strong);
  lua_rawget (L, LUA_REGISTRYINDEX);
  lua_pushlightuserdata (L, object);
  if (is_last_ref)
    {
      /* Remove from strong cache (i.e. assign nil to that slot). */
      lua_pushnil (L);
    }
  else
    {
      /* Find proxy object in the weak table and assign it to the
	 strong table. */
      lua_pushlightuserdata (L, &cache_weak);
      lua_rawget (L, LUA_REGISTRYINDEX);
      lua_pushvalue (L, -2);
      lua_rawget (L, -2);
      lua_replace (L, -2);
    }

  /* Store new value to the strong cache. */
  lua_rawset (L, -3);
  lua_pop (L, 1);
  lgi_callback_leave (data);
}

/* Retrieves requested typetable function for the object. */
static gpointer
object_load_function (lua_State *L, GType gtype, const gchar *name)
{
  gpointer func = NULL;
  if (object_type (L, gtype) != G_TYPE_INVALID)
    {
      func = lgi_gi_load_function (L, -1, name);
      lua_pop (L, 1);
    }
  return func;
}

/* Adds one reference to the object, returns TRUE if succeded. */
static gboolean
object_refsink (lua_State *L, gpointer obj)
{
  GType gtype = G_TYPE_FROM_INSTANCE (obj);
  if (G_TYPE_IS_OBJECT (gtype))
    {
      g_object_ref_sink (obj);
      return TRUE;
    }

  /* Check whether object has registered fundamental 'ref'
     function. */
  GIObjectInfo *info = g_irepository_find_by_gtype (NULL, gtype);
  if (info == NULL)
    info = g_irepository_find_by_gtype (NULL, G_TYPE_FUNDAMENTAL (gtype));
  if (info != NULL)
    {
      GIObjectInfoRefFunction ref =
	g_object_info_get_ref_function_pointer (info);
      g_base_info_unref (info);
      if (ref != NULL)
	{
	  ref (obj);
	  return TRUE;
	}
    }

  /* Finally check custom _refsink method in typetable. */
  gpointer (*refsink_func)(gpointer) =
    object_load_function (L, gtype, "_refsink");
  if (refsink_func)
    {
      refsink_func (obj);
      return TRUE;
    }

  /* There is no known wasy how to ref this kind of object. */
  g_warning ("no way to ref type `%s'", g_type_name (gtype));
  return FALSE;
}

/* Removes one reference from the object. */
static void
object_unref (lua_State *L, gpointer obj, gboolean remove_proxy)
{
  GType gtype = G_TYPE_FROM_INSTANCE (obj);
  if (G_TYPE_IS_OBJECT (gtype))
    {
      if (remove_proxy)
	g_object_remove_toggle_ref (obj, object_toggle_notify,
				    lgi_callback_context (L));
      else
	g_object_unref (obj);
      return;
    }

  /* Some other fundamental type, check, whether it has registered
     custom unref method. */
  GIObjectInfo *info = g_irepository_find_by_gtype (NULL, gtype);
  if (info == NULL)
    info = g_irepository_find_by_gtype (NULL, G_TYPE_FUNDAMENTAL (gtype));
  if (info != NULL)
    {
      GIObjectInfoUnrefFunction unref =
	g_object_info_get_unref_function_pointer (info);
      g_base_info_unref (info);
      if (unref != NULL)
	{
	  unref (obj);
	  return;
	}
    }

  void (*unref_func)(gpointer) = object_load_function (L, gtype, "_unref");
  if (unref_func)
    {
      unref_func (obj);
      return;
    }

  g_warning ("no way to unref type `%s'", g_type_name (gtype));
}

static int
object_gc (lua_State *L)
{
  object_unref (L, object_get (L, 1), TRUE);
  return 0;
}

static int
object_tostring (lua_State *L)
{
  gpointer obj = object_get (L, 1);
  GType gtype = G_TYPE_FROM_INSTANCE (obj);
  if (object_type (L, gtype) != G_TYPE_INVALID)
    lua_getfield (L, -1, "_name");
  else
    lua_pushliteral (L, "<??\?>");
  lua_pushfstring (L, "lgi.obj %p:%s(%s)", obj, lua_tostring (L, -1),
		   g_type_name (gtype));
  return 1;
}

gpointer
lgi_object_2c (lua_State *L, int narg, GType gtype, gboolean optional,
	       gboolean nothrow)
{
  gpointer obj;

  /* Check for nil. */
  if (optional && lua_isnoneornil (L, narg))
    return NULL;

  /* Get instance and perform type check. */
  obj = object_check (L, narg);
  if (!nothrow
      && (!obj || (gtype != G_TYPE_INVALID
		   && !g_type_is_a (G_TYPE_FROM_INSTANCE (obj), gtype))))
    object_type_error (L, narg, gtype);

  return obj;
}

int
lgi_object_2lua (lua_State *L, gpointer obj, gboolean own)
{
  GType gtype;

  /* NULL pointer results in nil. */
  if (!obj)
    {
      lua_pushnil (L);
      return 1;
    }

  /* Check, whether the object is already created (in the cache). */
  luaL_checkstack (L, 6, "");
  lua_pushlightuserdata (L, &cache_weak);
  lua_rawget (L, LUA_REGISTRYINDEX);
  lua_pushlightuserdata (L, obj);
  lua_rawget (L, -2);
  if (!lua_isnil (L, -1))
    {
      /* Use the object from the cache. */
      lua_replace (L, -2);

      /* If the object was already owned, remove one reference,
	 because our proxy always keeps only one reference, which we
	 already have. */
      if (own)
	object_unref (L, obj, FALSE);
      return 1;
    }

  /* Create new userdata object and attach empty table as its environment. */
  *(gpointer *) lua_newuserdata (L, sizeof (obj)) = obj;
  lua_pushlightuserdata (L, &object_mt);
  lua_rawget (L, LUA_REGISTRYINDEX);
  lua_setmetatable (L, -2);
  lua_newtable (L);
  lua_setfenv (L, -2);

  /* Store newly created userdata proxy into weak cache. */
  lua_pushlightuserdata (L, obj);
  lua_pushvalue (L, -2);
  lua_rawset (L, -5);

  /* Stack cleanup, remove unnecessary weak cache and nil under userdata. */
  lua_replace (L, -3);
  lua_pop (L, 1);

  /* If we don't own the object, take its ownership (and also remove
     floating reference if there is any). */
  gtype = G_TYPE_FROM_INSTANCE (obj);
  if (!own && object_refsink (L, obj))
    own = TRUE;

  if (G_TYPE_IS_OBJECT (gtype))
    {
      /* Create toggle reference and add object to the strong cache. */
      gpointer user_data = lgi_callback_context (L);
      g_object_add_toggle_ref (obj, object_toggle_notify, user_data);
      object_toggle_notify (user_data, obj, FALSE);

      /* If the object was already pre-owned, remove one reference
	 (because we have added one owning toggle reference). */
      if (own)
	g_object_unref (obj);
    }

  return 1;
}

/* Worker method for __index and __newindex implementation. */
static int
object_access (lua_State *L)
{
  gboolean getmode = lua_isnone (L, 3);

  /* Check that 1st arg is an object and invoke one of the forms:
     result = type:_access(objectinstance, name)
     type:_access(objectinstance, name, val) */
  gpointer object = object_get (L, 1);
  GType gtype = G_TYPE_FROM_INSTANCE (object);
  if (object_type (L, gtype) == G_TYPE_INVALID)
    object_type_error (L, 1, gtype);
  return lgi_marshal_access (L, getmode, 1, 2, 3);
}

/* Registration table. */
static const luaL_Reg object_mt_reg[] = {
  { "__gc", object_gc },
  { "__tostring", object_tostring },
  { "__index", object_access },
  { "__newindex", object_access },
  { NULL, NULL }
};

static const char *const query_mode[] = {
  "gtype", "repo", "class", "env", NULL
 };

/* Queries for assorted instance properties. Lua-side prototype:
   res = object.query(objectinstance, mode [, iface-gtype])
   Supported mode strings are:
   'gtype': returns real gtype of this instance.
   'repo':  returns repotable for this instance.
   'class': returns class struct record of this instance.
   'env':   returns environment table associated with the object. */
static int
object_query (lua_State *L)
{
  gpointer object = object_check (L, 1);
  if (object)
    {
      int mode = luaL_checkoption (L, 2, query_mode[0], query_mode);
      GType gtype = lgi_type_get_gtype (L, 3);
      if (gtype == G_TYPE_INVALID)
	gtype = G_TYPE_FROM_INSTANCE (object);
      if (mode == 0)
	{
	  lua_pushnumber (L, gtype);
	  return 1;
	}
      else if (mode == 3)
	{
	  lua_getfenv (L, 1);
	  return 1;
	}
      else
	{
	  /* Get repotype structure. */
	  if (object_type (L, gtype) != G_TYPE_INVALID)
	    {
	      if (mode == 2)
		{
		  gpointer typestruct = !G_TYPE_IS_INTERFACE (gtype)
		    ? G_TYPE_INSTANCE_GET_CLASS (object, gtype, GTypeClass)
		    : G_TYPE_INSTANCE_GET_INTERFACE (object, gtype, GTypeClass);
		  lua_getfield (L, -1, "_class");
		  lgi_record_2lua (L, typestruct, FALSE, 0);
		}
	      return 1;
	    }
	}
    }
  return 0;
}

/* Object field accessor.  Lua-side prototypes:
   res = object.field(objectinstance, gi.fieldinfo)
   object.field(objectinstance, gi.fieldinfo, newvalue) */
static int
object_field (lua_State *L)
{
  /* Check, whether we are doing set or get operation. */
  gboolean getmode = lua_isnone (L, 3);

  /* Get object instance. */
  gpointer object = object_get (L, 1);

  /* Call field marshalling worker. */
  return lgi_marshal_field (L, object, getmode, 1, 2, 3);
}

/* Object API table. */
static const luaL_Reg object_api_reg[] = {
  { "query", object_query },
  { "field", object_field },
  { NULL, NULL }
};

void
lgi_object_init (lua_State *L)
{
  /* Register metatable. */
  lua_pushlightuserdata (L, &object_mt);
  lua_newtable (L);
  luaL_register (L, NULL, object_mt_reg);
  lua_rawset (L, LUA_REGISTRYINDEX);

  /* Initialize caches. */
  lgi_cache_create (L, &cache_weak, "v");
  lgi_cache_create (L, &cache_strong, NULL);

  /* Create object API table and set it to the parent. */
  lua_newtable (L);
  luaL_register (L, NULL, object_api_reg);
  lua_setfield (L, -2, "object");
}
