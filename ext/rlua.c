/**
 * Copyright (C) 2004 Gregoire Lejeune <gregoire.lejeune@free.fr>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#include "rlua.h"

VALUE mLanguage;
VALUE cLua;

VALUE ruby_lua_stack_size(VALUE self);
VALUE ruby_lua_stack_dump(VALUE self);
VALUE ruby_lua_pop(VALUE self);

#define _W_STACK(L) {\
    int i;\
    int iStackSize = lua_gettop(L);\
    rb_warn("Lua stack size = %d", iStackSize);\
    \
    for (i = 1; i <= iStackSize; i++) {\
      int t = lua_type(L, i);\
      switch (t) {\
        case LUA_TSTRING:  /* strings */\
          printf("`%s'", lua_tostring(L, i));\
          break;\
      \
        case LUA_TBOOLEAN:  /* booleans */\
          printf(lua_toboolean(L, i) ? "true" : "false");\
          break;\
      \
        case LUA_TNUMBER:  /* numbers */\
          printf("%g", lua_tonumber(L, i));\
          break;\
      \
        default:  /* other values */\
          printf("%s", lua_typename(L, t));\
          break;\
      }\
      printf("  ");  /* put a separator */\
    }\
    printf("\n");  /* end the listing */                \
\
}

static void *l_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;  (void)osize;  /* not used */
  if (nsize == 0) {
    free(ptr);
    return NULL;
  } else {
		return realloc(ptr, nsize);
	}
}

static const char *_xSelf = "_xSelf";

/**
 * lua_SetVarToStack(lua_State *L, VALUE vVarValue)
 *
 * Desc: Positionne une valeur Ruby dans la pile Lua
 */
static void lua_SetVarToStack(lua_State *L, VALUE vVarValue) {
  int iStackSize;
  long iRbArrayLength;
  int iCpt;
  VALUE keys;

  switch(TYPE(vVarValue)) {
    case T_NIL:
      lua_pushnil(L);
      break;
    case T_TRUE:
      lua_pushboolean(L, 1);
      break;
    case T_FALSE:
      lua_pushboolean(L, 0);
      break;
    case T_SYMBOL:
      vVarValue = rb_str_new_cstr(rb_id2name(SYM2ID(vVarValue)));
      lua_pushlstring(L, RSTRING_PTR(vVarValue), RSTRING_LEN(vVarValue));
      break;
    case T_STRING:
      lua_pushlstring(L, RSTRING_PTR(vVarValue), RSTRING_LEN(vVarValue));
      break;
    case T_FIXNUM:
      lua_pushnumber(L, FIX2INT(vVarValue));
      break;
    case T_BIGNUM:
      lua_pushnumber(L, NUM2DBL(vVarValue));
      break;
    case T_FLOAT:
      lua_pushnumber(L, (lua_Number)RFLOAT_VALUE(vVarValue));
      break;
    case T_ARRAY:
      lua_newtable(L);
      iStackSize = lua_gettop(L);
      iRbArrayLength = RARRAY_LEN(vVarValue);
      for(iCpt = 0; iCpt < iRbArrayLength; iCpt++) {
        lua_SetVarToStack(L, RARRAY_PTR(vVarValue)[iCpt]);
        lua_rawseti(L, iStackSize, iCpt + 1);
      }
      break;
    case T_HASH:
      lua_newtable(L);
      keys = rb_funcall(vVarValue, rb_intern("keys"), 0);
      for(iCpt = 0; iCpt <= RARRAY_LEN(keys)-1; iCpt++){
        VALUE vHashKey;
        vHashKey = *(RARRAY_PTR(keys)+iCpt);

        if (TYPE(vHashKey) == T_TRUE
          || TYPE(vHashKey) == T_FALSE
          || TYPE(vHashKey) == T_STRING
          || TYPE(vHashKey) == T_SYMBOL
          || TYPE(vHashKey) == T_FIXNUM
          || TYPE(vHashKey) == T_BIGNUM
          || TYPE(vHashKey) == T_FLOAT)
          lua_SetVarToStack(L, vHashKey);
        else
          rb_raise(rb_eTypeError, "Invalid hash key data type %+"PRIsVALUE, vHashKey);
        
        lua_SetVarToStack(L, rb_hash_aref(vVarValue, vHashKey));
        lua_settable(L, -3);
      }
      break;
    default:
      lua_pushlightuserdata(L,(void*)vVarValue); /* saves ruby object */
      break;
  }
}

/**
 * lua_SetNamedVarFromRuby(lua_State *L, VALUE vVarName, VALUE vVarValue);
 *
 * Desc: Positionne une valeur nomm�e Ruby dans l'index global Lua
 */
static void lua_SetNamedVarFromRuby(lua_State *L, VALUE vVarName, VALUE vVarValue) {
#ifdef HAVE_LUA_GLOBALSINDEX
  lua_pushlstring(L, RSTRING_PTR(vVarName), RSTRING_LEN(vVarName));
#else
  char * var_name = (char*)malloc(sizeof(char)*(RSTRING_LEN(vVarName)+1));
  memset(var_name, 0, RSTRING_LEN(vVarName)+1);
  memcpy(var_name, RSTRING_PTR(vVarName), RSTRING_LEN(vVarName));
  lua_pushstring(L, var_name);
#endif
  lua_SetVarToStack(L, vVarValue);
#ifdef HAVE_LUA_GLOBALSINDEX
  lua_settable(L, LUA_GLOBALSINDEX);
#else
  lua_setglobal(L, var_name);
  free(var_name);
#endif
  return;
}

/**
 * VALUE v = lua_GetVarFromStack(lua_State *L);
 *
 * Desc: R�cup�re la valeur au sommet de la pile Lua
 */
static VALUE lua_GetVarFromStack(lua_State *L) {
  int iStackSize = 0;
  VALUE vTableKey = Qnil;
  VALUE vTableValue = Qnil;
  VALUE RCod = Qnil;
  int iK = 0;
  int iIsArray = 1;

  rb_require("language/lua/function");

  switch(lua_type(L,-1)){
    case LUA_TNONE:
      break;
    case LUA_TBOOLEAN:
      RCod = lua_toboolean(L,-1) ? Qtrue : Qfalse;
      break;
    case LUA_TUSERDATA:
      RCod = (VALUE) lua_touserdata(L,-1);
      break;
    case LUA_TNIL:
      break;
    case LUA_TNUMBER:
      RCod = rb_float_new(lua_tonumber(L,-1));
      break;
    case LUA_TSTRING:
      RCod = rb_str_new2(lua_tostring(L,-1));
      break;
    case LUA_TTABLE:
      RCod = rb_hash_new();
      iStackSize = lua_gettop(L);
      iK = 0;
      lua_pushnil(L);
      while(lua_next(L,iStackSize) != 0){
        vTableValue = lua_GetVarFromStack(L);
        lua_pop(L,1);
        vTableKey = lua_GetVarFromStack(L);
        if (TYPE(vTableKey) != T_FLOAT 
          || floor(NUM2DBL(vTableKey)) != NUM2DBL(vTableKey)
          || NUM2INT(vTableKey) != ++iK) {
          iIsArray = 0;
        }

        if (TYPE(vTableKey) == T_STRING && strcmp(StringValueCStr(vTableKey), "__classname") == 0) {
          // omit "__classname" key
        } else {
          rb_hash_aset(RCod,vTableKey,vTableValue);
        }
      }

      if (iIsArray > 0)
        RCod = rb_funcall(RCod, rb_intern("values"), 0);

      lua_settop(L,iStackSize);
      break;
    case LUA_TFUNCTION:
      RCod = rb_class_new_instance(0, NULL, rb_const_get(cLua, rb_intern("Function")));
      break;
    default:
      RCod = (VALUE)lua_touserdata(L, -1);
      break;
  }
  return(RCod);
}

typedef struct populate {
  VALUE lua;
  char *parent;
} populate;
void lua_populate_fun(VALUE table, VALUE lua, char *parent);

int do_populate_lua(VALUE key, VALUE val, VALUE in) {
  if(T_STRING == TYPE(key)) {
    char *fn = concatenate(3, ((populate*)in)->parent, ".", StringValuePtr(key));

    switch(TYPE(val)) {
      case T_OBJECT:
        if(0 == strcmp("Language::Lua::Function", rb_obj_classname(val))) {
          rb_ivar_set(val, rb_intern("@lua"), ((populate*)in)->lua);
          rb_ivar_set(val, rb_intern("@fname"), rb_str_new2(fn));
        }
        break;
      case T_HASH:
        lua_populate_fun(val, ((populate*)in)->lua, fn);
        break;
      default:
        break;
    }

    free(fn);
  }
  return ST_CONTINUE;
}

void lua_populate_fun(VALUE table, VALUE lua, char *parent) {
  populate *p = (populate*)malloc(sizeof(populate));
  p->lua = lua;
  p->parent = parent;

  if(T_HASH != TYPE(table)) {
    return;
  }
  rb_hash_foreach(table, do_populate_lua, (VALUE)p);

  free(p);
}

static int lua_CallRubyFunction(lua_State *L) {
  int iNbArgs = lua_gettop(L);
  VALUE self;
  char *xRubyFuncName;
  ID idRubyFunc;
  int iNbFuncArgs = 0;
  VALUE	*vFuncArgs;
  VALUE vFuncRCod = Qnil;
  int iCpt;

  if(iNbArgs < 1 || !lua_isstring(L, 1)) {
    lua_pushstring(L, "incorrect argument to function `ruby'");
    lua_error(L);
  }

  xRubyFuncName = (char *)lua_tostring(L, 1);
  idRubyFunc = rb_intern(xRubyFuncName);
  iNbFuncArgs = iNbArgs - 1;
  lua_remove(L, 1);

  /** Act: Recuperation de self depuis la registry Lua */
  lua_pushlightuserdata(L, (void *)&_xSelf);
  lua_gettable(L, LUA_REGISTRYINDEX);
  self = (VALUE)lua_touserdata(L,-1);
  lua_pop(L, 1);

  vFuncArgs = malloc(sizeof(VALUE) * iNbFuncArgs);
  for(iCpt = 0; iCpt < iNbFuncArgs; iCpt++) {
	  vFuncArgs[iNbFuncArgs - iCpt - 1] = lua_GetVarFromStack(L);
	  lua_remove(L, -1);
  }

  vFuncRCod = rb_funcall2(self, idRubyFunc, iNbFuncArgs, vFuncArgs);
  lua_SetVarToStack(L, vFuncRCod);
  
  return 1;
}

/** ------------------------------------------------------------------------ */

/**
 * VALUE v = ruby_CallLuaFunction(char *xFunctionName, int iNbArgs, VALUE *vArgs, VALUE self)
 *
 * Desc: Appel la fonction Lua xFunctionName avec les param�tres contenu dans vArgs
 *       et retourne le r�sultat
 */
static VALUE ruby_CallLuaFunction(char *xFunctionName, int iNbArgs, VALUE *vArgs, VALUE self) {
  RbTlua *pRbTlua;
  int iCpt;
  int stack_size, final_stack_size, final_pop;
  char *point_in_fun;
  VALUE RCod = Qnil;
  VALUE RCrt = Qnil;
  int iFuncLevel = 0;

  Data_Get_Struct(self, RbTlua, pRbTlua);

  stack_size = lua_gettop(pRbTlua->L);

  lua_getglobal(pRbTlua->L, xFunctionName);  /* get function */
  if(!lua_isfunction(pRbTlua->L,lua_gettop(pRbTlua->L)) || NULL == strchr(xFunctionName, '.')){
    lua_pop(pRbTlua->L, 1);

    point_in_fun = strtok(xFunctionName, ".");
    if(NULL == point_in_fun) {
      rb_raise(rb_eSystemCallError, "error running lua function `%s': table does not exist", xFunctionName);
    } else {
      lua_getglobal(pRbTlua->L, point_in_fun);
    }
    point_in_fun = strtok(NULL, ".");
    while(NULL != point_in_fun) {
      lua_getfield(pRbTlua->L, -1, point_in_fun);
      iFuncLevel++;
      point_in_fun = strtok(NULL, ".");
    }
  } else {
    rb_raise(rb_eSystemCallError, "error running lua function `%s': does not exist", xFunctionName);
  }

  for(iCpt = 0; iCpt < iNbArgs; iCpt++) {
    lua_SetVarToStack(pRbTlua->L, vArgs[iCpt]);
  }

  if(lua_pcall(pRbTlua->L, iNbArgs, -1, 0) != 0)  /* do the call */ {
    rb_raise(rb_eSystemCallError, "error running lua function `%s': %s", xFunctionName, lua_tostring(pRbTlua->L, -1));
  }

  final_stack_size = lua_gettop(pRbTlua->L);
  final_pop = final_stack_size - stack_size - iFuncLevel;

  if (final_pop == 0) {
    RCod = Qnil;
  }  else if (final_pop == 1) {
    RCod = lua_GetVarFromStack(pRbTlua->L);
    lua_pop(pRbTlua->L, final_pop);
    lua_SetVarToStack(pRbTlua->L, RCod);
  } else {
    RCod = rb_ary_new();
    for(iCpt = 0; iCpt < final_pop; iCpt++) {
      RCrt = lua_GetVarFromStack(pRbTlua->L);
      lua_pop(pRbTlua->L, 1);
      rb_ary_unshift(RCod, RCrt);
    }

    for(iCpt = 0; iCpt < final_pop; iCpt++)
      lua_SetVarToStack(pRbTlua->L, RARRAY_PTR(RCod)[iCpt]);
  }

  lua_pop(pRbTlua->L, iFuncLevel);

  return(RCod);
}

/** ------------------------------------------------------------------------ */

/**
 * Desc: Destructeur de class
 */
void ruby_lua_free(RbTlua *pRbTlua) {
  if (pRbTlua != NULL) {
    lua_close(pRbTlua->L);
    free(pRbTlua);
  }
}

/**
 * Desc: mark
 */
void ruby_lua_mark(RbTlua *pRbTlua) {
  if(pRbTlua == NULL) return;
//  if(!NIL_P(pRbTlua->xXmlData)) rb_gc_mark(pRbTlua->xXmlData);

  return;
}

/** 
 * o = Language::Lua.new() 
 *
 * Desc: Constructeur de class
 */
VALUE ruby_lua_new(VALUE class) {
  VALUE self; 
  RbTlua *pRbTlua;

  pRbTlua = (RbTlua *)malloc(sizeof(RbTlua));
  if(pRbTlua == NULL)
    rb_raise(rb_eNoMemError, "No memory left for Lua struct");
      
  pRbTlua->L = lua_newstate(l_alloc, pRbTlua->ud);
  lua_register(pRbTlua->L, "ruby", lua_CallRubyFunction);
	luaL_openlibs(pRbTlua->L);
  
  self = Data_Wrap_Struct(class, ruby_lua_mark, ruby_lua_free, pRbTlua);
  
  /** Act: On place self dans la Registry Lua */
  lua_pushlightuserdata(pRbTlua->L, (void *)&_xSelf);
  lua_pushlightuserdata(pRbTlua->L, (void *)self);
  lua_settable(pRbTlua->L, LUA_REGISTRYINDEX);

	if(rb_block_given_p()) {
		rb_yield(self);
	}

  return(self);
}  

/** 
 * lua.load("script.lua") 
 * lua.dofile("script.lua") 
 *
 * Desc: Charge un script Lua
 */
VALUE ruby_lua_load(VALUE self, VALUE xOutFilename) {
  RbTlua *pRbTlua;
  Data_Get_Struct(self, RbTlua, pRbTlua);

  if(luaL_loadfile(pRbTlua->L, StringValuePtr(xOutFilename)) || lua_pcall(pRbTlua->L, 0, 0, 0)) {
    rb_raise(rb_eSystemCallError, "cannot run file %s !", lua_tostring(pRbTlua->L, -1));
  }

  return(Qnil);
}

/** 
 * v = lua.<lua_method>(...) 
 *
 * Desc: Appel la fonction Lua <lua_method> et renvoi le r�sultat 
 *       en le supprimant de la stack
 */
VALUE ruby_lua_method_missing(int iNbArgs, VALUE *vArgs, VALUE self) {
  VALUE RCod = Qnil;
  RbTlua *pRbTlua;
  Data_Get_Struct(self, RbTlua, pRbTlua);

  RCod = ruby_CallLuaFunction((char*)rb_id2name(SYM2ID(vArgs[0])), iNbArgs - 1, vArgs + 1, self);
  lua_pop(pRbTlua->L, 1);

  return(RCod);
}

/**
 * v = lua.call([true|false], "<lua_method>", ...)
 *
 * Desc: Appel la fonction Lua <lua_method> et renvoi le r�sultat 
 *       en le conservant dant la stack si le premier argument est true 
 */
VALUE ruby_lua_call(int iNbArgs, VALUE *vArgs, VALUE self) {
  VALUE RCod = Qnil;
  RbTlua *pRbTlua;
  Data_Get_Struct(self, RbTlua, pRbTlua);

  if(iNbArgs > 0) {
    switch(TYPE(vArgs[0])) {
      case T_TRUE:
        RCod = ruby_CallLuaFunction(StringValuePtr(vArgs[1]), iNbArgs - 2, vArgs + 2, self);
        break;

      case T_FALSE:
        RCod = ruby_CallLuaFunction(StringValuePtr(vArgs[1]), iNbArgs - 2, vArgs + 2, self);
        lua_pop(pRbTlua->L, 1);
        break;

      case T_STRING:
        RCod = ruby_CallLuaFunction(StringValuePtr(vArgs[0]), iNbArgs - 1, vArgs + 1, self);
        lua_pop(pRbTlua->L, 1);
        break;

      default:
        rb_raise(rb_eArgError, "Language::Lua.call argument error !");
    }
  } else {
    int i, j, k;
    for(i = -1, j = 0; j < NUM2INT(ruby_lua_stack_size(self)) && LUA_TLIGHTUSERDATA != lua_type(pRbTlua->L, i); i--, j++);
    if(j < NUM2INT(ruby_lua_stack_size(self))) {
      VALUE fun = (VALUE)lua_touserdata(pRbTlua->L, i);
      if(0 == strcmp("Language::Lua::Function", rb_obj_classname(fun))) {
        VALUE result;

        VALUE fname = rb_ivar_get(fun, rb_intern("@fname"));
        int nb_args = abs(i + 1);
        VALUE *args = (VALUE*)malloc(sizeof(VALUE) * (nb_args + 1));
        
        for(k = nb_args; k > 0; k--) {
          args[k] = ruby_lua_pop(self);
        }
        args[0] = fname;
        
        lua_remove(pRbTlua->L, 1);

        result = ruby_lua_call(nb_args + 1, args, self); 
        free(args);
        return result;
      }
    } else {
      rb_raise(rb_eArgError, "Language::Lua.call argument error !");
    }
  }

  return(RCod);
}

/**
 * v = lua.var([true|false], "<lua_varname>", [<value>])
 *
 * Desc: Retourne la valeur de la valiable Lua <lua_varname>
 *       en le conservant dans la stack si le premier argument est true .
 *       Ou positionne la valeur de 
 */
VALUE ruby_lua_var(int iNbArgs, VALUE *vArgs, VALUE self) {
  VALUE RCod = Qnil;
  RbTlua *pRbTlua;
  Data_Get_Struct(self, RbTlua, pRbTlua);

  switch(TYPE(vArgs[0])) {
    case T_TRUE:
      if(iNbArgs == 2) {
        /** Act: Recup�re la valeur */
        lua_getglobal(pRbTlua->L, StringValuePtr(vArgs[1]));
        RCod = lua_GetVarFromStack(pRbTlua->L);
        lua_populate_fun(RCod, self, StringValuePtr(vArgs[1]));
      } else if(iNbArgs == 3) {
        /** Positionne la valeur */
        lua_SetNamedVarFromRuby(pRbTlua->L, vArgs[1], vArgs[2]);
      } else {
        rb_raise(rb_eArgError, "Language::Lua.var argument error !");
      }
      break;
      
    case T_FALSE:
      if(iNbArgs == 2) {
        /** Act: Recup�re la valeur */
        lua_getglobal(pRbTlua->L, StringValuePtr(vArgs[1]));
        RCod = lua_GetVarFromStack(pRbTlua->L);
        lua_populate_fun(RCod, self, StringValuePtr(vArgs[1]));
        lua_pop(pRbTlua->L, 1);
      } else if(iNbArgs == 3) {
        /** Positionne la valeur */
        lua_SetNamedVarFromRuby(pRbTlua->L, vArgs[1], vArgs[2]);
      } else {
        rb_raise(rb_eArgError, "Language::Lua.var argument error !");
      }
      break;
      
    case T_STRING:
      if(iNbArgs == 1) {
        /** Act: Recup�re la valeur */
        lua_getglobal(pRbTlua->L, StringValuePtr(vArgs[0]));
        RCod = lua_GetVarFromStack(pRbTlua->L);
        lua_populate_fun(RCod, self, StringValuePtr(vArgs[0]));
        lua_pop(pRbTlua->L, 1);
      } else if(iNbArgs == 2) {
        /** Positionne la valeur */
        lua_SetNamedVarFromRuby(pRbTlua->L, vArgs[0], vArgs[1]);
      } else {
        rb_raise(rb_eArgError, "Language::Lua.var argument error !");
      }
      break;
    
    default:
      rb_raise(rb_eArgError, "Language::Lua.var argument error !");
  }

  return(RCod);
}

/** 
 * lua.eval("<lua_code>") 
 *
 * Desc: Evalue le code Lua <lua_code>
 */
VALUE ruby_lua_eval(VALUE self, VALUE xLuaCode) {
  char *pszLuaCode; 
  RbTlua *pRbTlua;
  Data_Get_Struct(self, RbTlua, pRbTlua);
  
  pszLuaCode = StringValuePtr(xLuaCode);
  if(luaL_loadbuffer(pRbTlua->L, pszLuaCode, strlen(pszLuaCode), "line") || lua_pcall(pRbTlua->L, 0, 0, 0)) {
    rb_raise(rb_eSystemCallError, "Lua error: %s", lua_tostring(pRbTlua->L, -1));
  }

  return(Qnil);
}

VALUE ruby_lua_pop(VALUE self) {
  RbTlua *pRbTlua;
  VALUE result = Qnil;

  Data_Get_Struct(self, RbTlua, pRbTlua);

  if(NUM2INT(ruby_lua_stack_size(self)) > 0) {
    result = lua_GetVarFromStack(pRbTlua->L);
    lua_pop(pRbTlua->L, 1);
  }

  return result;
}

VALUE ruby_lua_push(VALUE self, VALUE value) {
  RbTlua *pRbTlua;
  Data_Get_Struct(self, RbTlua, pRbTlua);

  lua_SetVarToStack(pRbTlua->L, value);
  return Qnil;
}

/**
 * v = lua.stackSize()
 *
 * Desc: Retourne la taille de la stack Lua
 */
VALUE ruby_lua_stack_size(VALUE self) {
  RbTlua *pRbTlua;
  Data_Get_Struct(self, RbTlua, pRbTlua);
  
  return(INT2NUM(lua_gettop(pRbTlua->L)));
}

/**
 * lua.stackDump()
 *
 * Desc: Dump le contenu de la stack sur la sortie standard
 */
VALUE ruby_lua_stack_dump(VALUE self) {
  RbTlua *pRbTlua;
  int i;
  int iStackSize;
  
  Data_Get_Struct(self, RbTlua, pRbTlua);
  
  iStackSize = lua_gettop(pRbTlua->L);
  if(0 == iStackSize) {
    printf("## empty stack\n");
  }
  for (i = 1; i <= iStackSize; i++) {  /* repeat for each level */
    int t = lua_type(pRbTlua->L, i);
    switch (t) {
      case LUA_TSTRING:  /* strings */
        printf("%04d: `%s'\n", i, lua_tostring(pRbTlua->L, i));
        break;
    
      case LUA_TBOOLEAN:  /* booleans */
        printf("%04d: %s\n", i, (lua_toboolean(pRbTlua->L, i) ? "true" : "false"));
        break;
    
      case LUA_TNUMBER:  /* numbers */
        printf("%04d: %g\n", i, lua_tonumber(pRbTlua->L, i));
        break;
    
      default:  /* other values */
        printf("%04d: #%s\n", i, lua_typename(pRbTlua->L, t));
        break;
    }
  }

  return(Qnil);
}

/** ------------------------------------------------------------------------ */

void Init_lua(void) {
  mLanguage  = rb_define_module("Language");
  cLua = rb_define_class_under(mLanguage, "Lua", rb_cObject);

  rb_define_const(cLua, "RUBY_LUA_VERSION", rb_str_new2(RUBY_LUA_VERSION));

  rb_define_singleton_method(cLua, "new", ruby_lua_new, 0);

  rb_define_method(cLua, "load",           ruby_lua_load,            1);
  rb_define_method(cLua, "dofile",         ruby_lua_load,            1);
  rb_define_method(cLua, "method_missing", ruby_lua_method_missing, -1);
  rb_define_method(cLua, "call",           ruby_lua_call,           -1);
  rb_define_method(cLua, "var",            ruby_lua_var,            -1);
  rb_define_method(cLua, "eval",           ruby_lua_eval,            1);
  rb_define_method(cLua, "pop",            ruby_lua_pop,             0);
  rb_define_method(cLua, "push",           ruby_lua_push,            1);
  
  rb_define_method(cLua, "stack_size",      ruby_lua_stack_size,      0);
  rb_define_method(cLua, "stack_dump",      ruby_lua_stack_dump,      0);
}

