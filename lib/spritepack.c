#include "spritepack.h"
#include "matrix.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdint.h>
#include <string.h>

#define TAG_ID 1
#define TAG_COLOR 2
#define TAG_ADDITIVE 4
#define TAG_MATRIX 8

struct import_alloc {
	lua_State *L;
	char * buffer;
	int cap;
};

static void *
ialloc(struct import_alloc *alloc, int size) {
	if (alloc->cap < size) {
		luaL_error(alloc->L, "import invalid stream, alloc failed");
	}
	void * ret = alloc->buffer;
	alloc->buffer += size;
	alloc->cap -= size;
	return ret;
}

struct import_stream {
	struct import_alloc *alloc;
	struct sprite_pack *pack;
	const char * stream;
	size_t size;
};

static int
import_byte(struct import_stream *is) {
	if (is->size == 0) {
		luaL_error(is->alloc->L, "Invalid import stream");
	}
	uint8_t c = (uint8_t)*(is->stream);
	++is->stream;
	--is->size;

	return c;
}

static int
import_word(struct import_stream *is) {
	if (is->size < 2) {
		luaL_error(is->alloc->L, "Invalid import stream");
	}
	uint8_t low = (uint8_t)*(is->stream);
	uint8_t high = (uint8_t)*(is->stream+1);
	is->stream += 2;
	is->size -= 2;

	return low | (uint32_t)high << 8;
}

static int32_t
import_int32(struct import_stream *is) {
	if (is->size < 4) {
		luaL_error(is->alloc->L, "Invalid import stream");
	}
	uint8_t b[4];
	b[0] = (uint8_t)*(is->stream);
	b[1] = (uint8_t)*(is->stream+1);
	b[2] = (uint8_t)*(is->stream+2);
	b[3] = (uint8_t)*(is->stream+3);
	is->stream += 4;
	is->size -= 4;

	return (int32_t)(b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24);
}

static uint32_t
import_color(struct import_stream *is) {
	if (is->size < 4) {
		luaL_error(is->alloc->L, "Invalid import stream");
	}
	uint8_t b[4];
	b[0] = (uint8_t)*(is->stream);
	b[1] = (uint8_t)*(is->stream+1);
	b[2] = (uint8_t)*(is->stream+2);
	b[3] = (uint8_t)*(is->stream+3);
	is->stream += 4;
	is->size -= 4;

	return (b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24);
}

static void
import_picture(struct import_stream *is) {
	int n = import_byte(is);
	struct pack_picture * pp = ialloc(is->alloc, sizeof(*pp) + (n-1) * sizeof(struct pack_quad));
	pp->n = n;
	int i,j;
	for (i=0;i<n;i++) {
		struct pack_quad * q = &pp->rect[i];
		q->texid = import_byte(is);
		for (j=0;j<8;j++) {
			q->texture_coord[j] = import_word(is);
		}
		for (j=0;j<8;j++) {
			q->screen_coord[j] = import_int32(is);
		}
	}
}

static void
import_polygon(struct import_stream *is) {
	int n = import_byte(is);
	struct pack_polygon * pp = ialloc(is->alloc, sizeof(*pp) + (n-1) * sizeof(struct pack_poly));
	pp->n = n;
	int i,j;
	for (i=0;i<n;i++) {
		struct pack_poly * p = &pp->poly[i];
		p->texid = import_byte(is);
		p->n = import_byte(is);
		p->texture_coord = ialloc(is->alloc, p->n * 2 * sizeof(uint16_t));
		p->screen_coord =  ialloc(is->alloc, p->n * 2 * sizeof(uint32_t));
		for (j=0;j<p->n*2;j++) {
			p->texture_coord[j] = import_word(is);
		}
		for (j=0;j<p->n*2;j++) {
			p->screen_coord[j] = import_int32(is);
		}
	}
}

static const char *
import_string(struct import_stream *is) {
	int n = import_byte(is);
	if (n==255) {
		return NULL;
	}
	if (is->size < n) {
		luaL_error(is->alloc->L, "Invalid stream : read string failed");
	}
	char * buf = ialloc(is->alloc, (n+1+3) & ~3);
	memcpy(buf, is->stream, n);
	buf[n] = 0;
	is->stream += n;
	is->size -= n;

	return buf;
}

static void
import_frame(struct pack_frame * pf, struct import_stream *is, int maxc) {
	int n = import_byte(is);
	int i;
	pf->part = ialloc(is->alloc, n * sizeof(struct pack_part));
	pf->n = n;
	for (i=0;i<n;i++) {
		struct pack_part *pp = &pf->part[i];
		int tag = import_byte(is);
		if (tag & TAG_ID) {
			pp->component_id = import_word(is);
			if (pp->component_id >= maxc) {
				luaL_error(is->alloc->L, "Invalid stream : frame part has wrong component id");
			}
		} else {
			luaL_error(is->alloc->L, "Invalid stream : frame part need an id");
		}
		if (tag & TAG_MATRIX) {
			pp->t.mat = ialloc(is->alloc, sizeof(struct matrix));
			int32_t *m = pp->t.mat->m;
			int j;
			for (j=0;j<6;j++) {
				m[j] = import_int32(is);
			}
		} else {
			pp->t.mat = NULL;
		}
		if (tag & TAG_COLOR) {
			pp->t.color = import_color(is);
		} else {
			pp->t.color = 0xffffffff;
		}
		if (tag & TAG_ADDITIVE) {
			pp->t.additive = import_color(is);
		} else {
			pp->t.additive = 0;
		}
	}
}

static void
import_animation(struct import_stream *is) {
	int component = import_word(is);
	struct pack_animation * pa = ialloc(is->alloc, sizeof(*pa) + (component-1) * sizeof(struct pack_component));
	pa->component_number = component;
	int i;
	for (i=0;i<component;i++) {
		int id = import_word(is);
		if (id < 0 || id >= is->pack->n) {
			luaL_error(is->alloc->L, "Invalid stream : wrong id %d", id);
		}
		pa->component[i].id = id;
		pa->component[i].name = import_string(is);
	}
	pa->action_number = import_word(is);
	pa->action = ialloc(is->alloc, sizeof(struct pack_action) * pa->action_number);
	int frame = 0;
	for (i=0;i<pa->action_number;i++) {
		pa->action[i].name = import_string(is);
		pa->action[i].number = import_word(is);
		pa->action[i].start_frame = frame;
		frame += pa->action[i].number;
	}
	pa->frame_number = import_word(is);
	pa->frame = ialloc(is->alloc, sizeof(struct pack_frame) * pa->frame_number);
	for (i=0;i<pa->frame_number;i++) {
		import_frame(&pa->frame[i], is, component);
	}
}

static void
import_sprite(struct import_stream *is) {
	int id = import_word(is);
	if (id <0 || id >= is->pack->n) {
		luaL_error(is->alloc->L, "Invalid stream : wrong id %d", id);
	}
	int type = import_byte(is);
	is->pack->type[id] = type;
	if (is->pack->data[id]) {
		luaL_error(is->alloc->L, "Invalid stream : duplicate id %d", id);
	}
	is->pack->data[id] = is->alloc->buffer;
	switch (type) {
	case TYPE_PICTURE:
		import_picture(is);
		break;
	case TYPE_ANIMATION:
		import_animation(is);
		break;
	case TYPE_POLYGON:
		import_polygon(is);
		break;
	default:
		luaL_error(is->alloc->L, "Invalid stream : Unknown type %d", type);
		break;
	}
}

/*
	table/number texture
	integer maxid
	integer size
	string data

	ret: userdata sprite_pack
 */
static int
limport(lua_State *L) {
	int max_id = luaL_checkinteger(L, 2); 
	int size = luaL_checkinteger(L, 3);
	int tex;
	int tt = lua_type(L,1);
	if (tt == LUA_TNUMBER) {
		tex = 1;
	} else {
		if (tt != LUA_TTABLE) {
			return luaL_error(L, "Need textures");
		}

		tex = lua_rawlen(L,1);
	}

	struct import_alloc alloc;
	alloc.L = L;
	alloc.buffer = lua_newuserdata(L, size);
	alloc.cap = size;

	struct sprite_pack *pack = ialloc(&alloc, sizeof(*pack) + (tex-1) * sizeof(int));
	pack->n = max_id + 1;
	int align_n = (pack->n + 3) & ~3;
	pack->type = ialloc(&alloc, align_n * sizeof(uint8_t));
	memset(pack->type, 0, align_n * sizeof(uint8_t));
	pack->data = ialloc(&alloc, pack->n * sizeof(void*));
	memset(pack->data, 0, pack->n * sizeof(void*));

	if (lua_istable(L,1)) {
		int i;
		for (i=0; i<tex; i++) {
			lua_rawgeti(L,1,i+1);
			pack->tex[i] = luaL_checkinteger(L, -1);
			lua_pop(L,1);
		}
	} else {
		pack->tex[0] = lua_tointeger(L,1);
	}

	struct import_stream is;
	is.alloc = &alloc;
	is.stream =luaL_checklstring(L, 4, &is.size);
	is.pack = pack;

	while (is.size != 0) {
		import_sprite(&is);
	}

	return 1;
}

static int
lpackbyte(lua_State *L) {
	int n = luaL_checkinteger(L, 1);
	if (n < 0 || n > 255) {
		return luaL_error(L, "pack byte %d", n);
	}
	uint8_t buf[1] = { (uint8_t)n };
	lua_pushlstring(L, (char *)buf, 1);
	return 1;
}

static int
lpackword(lua_State *L) {
	int n = luaL_checkinteger(L, 1);
	if (n < 0 || n > 0xffff) {
		return luaL_error(L, "pack word %d", n);
	}
	uint8_t buf[2] = { 
		(uint8_t)n&0xff , 
		(uint8_t)((n>>8) & 0xff) , 
	};
	lua_pushlstring(L, (char *)buf, 2);
	return 1;
}

static int
lpackint32(lua_State *L) {
	int32_t sn = luaL_checkinteger(L, 1);
	uint32_t n = (uint32_t) sn;
	uint8_t buf[4] = { 
		(uint8_t)n&0xff , 
		(uint8_t)((n>>8) & 0xff) , 
		(uint8_t)((n>>16) & 0xff) , 
		(uint8_t)((n>>24) & 0xff) , 
	};
	lua_pushlstring(L, (char *)buf, 4);
	return 1;
}

static int
lpackcolor(lua_State *L) {
	uint32_t n = luaL_checkunsigned(L,1);

	uint8_t buf[4] = { 
		(uint8_t)n&0xff , 
		(uint8_t)((n>>8) & 0xff) , 
		(uint8_t)((n>>16) & 0xff) , 
		(uint8_t)((n>>24) & 0xff) , 
	};
	lua_pushlstring(L, (char *)buf, 4);
	return 1;
}

static int
lpackstring(lua_State *L) {
	size_t sz = 0;
	const char *str = lua_tolstring(L,1,&sz);
	if (sz >= 255) {
		return luaL_error(L, "%s is too long", str);
	}
	if (str == NULL) {
		uint8_t buf[1] = { 255 };
		lua_pushlstring(L, (char *)buf, 1);
	} else {
		uint8_t buf[sz + 1];
		buf[0] = sz;
		memcpy(buf+1, str, sz);
		lua_pushlstring(L, (char *)buf, sz+1);
	}
	return 1;
}

static int
lpackframetag(lua_State *L) {
	const char * tagstr = luaL_checkstring(L,1);
	int i;
	int tag = 0;
	for (i=0;tagstr[i];i++) {
		switch(tagstr[i]) {
		case 'i':
			tag |= TAG_ID;
			break;
		case 'c':
			tag |= TAG_COLOR;
			break;
		case 'a':
			tag |= TAG_ADDITIVE;
			break;
		case 'm':
			tag |= TAG_MATRIX;
			break;
		default:
			return luaL_error(L, "Invalid tag %s", tagstr);
			break;
		}
	}
	uint8_t buf[1] = { (uint8_t)tag };
	lua_pushlstring(L, (char *)buf, 1);
	return 1;
}

static int
lpicture_size(lua_State *L) {
	int n = luaL_checkinteger(L,1);
	int sz = sizeof(struct pack_picture) + (n - 1) * sizeof(struct pack_quad);
	lua_pushinteger(L, sz);
	return 1;
}

static int
lpolygon_size(lua_State *L) {
	int n = luaL_checkinteger(L,1);
	int pn = luaL_checkinteger(L,2);
	int sz = sizeof(struct pack_polygon) 
		+ (n-1) * sizeof(struct pack_poly)
		+ 12 * pn;
	lua_pushinteger(L, sz);
	return 1;
}

static int
lpack_size(lua_State *L) {
	int max_id = luaL_checkinteger(L,1);
	int tex = luaL_checkinteger(L,2);
	int align_n = (max_id + 1 + 3) & ~3;
	int size = sizeof(struct sprite_pack) 
		+ align_n * sizeof(uint8_t) 
		+ (max_id+1) * sizeof(void *) 
		+ (tex-1) * sizeof(int);

	lua_pushinteger(L, size);

	return 1;
}

/*
	integer frame
	integer component
	integer action

	ret: integer size
 */
static int
lanimation_size(lua_State *L) {
	int frame = luaL_checkinteger(L,1);
	int component = luaL_checkinteger(L,2);
	int action = luaL_checkinteger(L,3);

	int size = sizeof(struct pack_animation) 
		+ frame * sizeof(struct pack_frame)
		+ action * sizeof(struct pack_action)
		+ (component-1) * sizeof(struct pack_component);

	lua_pushinteger(L, size);
	return 1;
}

static int
lpart_size(lua_State *L) {
	int size;
	if (lua_isnoneornil(L,1)) {
		size = sizeof(struct pack_part);
	} else {
		size = sizeof(struct pack_part) + sizeof(struct matrix);
	}
	lua_pushinteger(L, size);

	return 1;
}

static int
lstring_size(lua_State *L) {
	int size;
	if (lua_isnoneornil(L,1)) {
		size = 0;
	} else {
		size_t sz = 0;
		luaL_checklstring(L,1,&sz);
		size = (sz + 1 + 3) & ~3;
	}
	lua_pushinteger(L, size);
	return 1;
}

void 
dump_pack(struct sprite_pack *pack) {
	if (pack == NULL)
		return;
	int i;
	for (i=0;i<pack->n;i++) {
		if (pack->type[i] == TYPE_PICTURE)
			printf("%d : PICTURE\n", i);
		else {
			struct pack_animation *ani = pack->data[i];
			printf("%d : ANIMATION %d\n", i, ani->component_number);
			int i;
			for (i=0;i<ani->component_number;i++) {
				printf("\t%d %s\n",ani->component[i].id, ani->component[i].name ? ani->component[i].name : "");
			}
		}
	}
}

static int
ldumppack(lua_State *L) {
	dump_pack(lua_touserdata(L,1));
	return 0;
}

int
ejoy2d_spritepack(lua_State *L) {
	luaL_Reg l[] = {
		{ "byte", lpackbyte },
		{ "word", lpackword },
		{ "int32", lpackint32 },
		{ "color", lpackcolor },
		{ "string", lpackstring },
		{ "frametag", lpackframetag },
		{ "picture_size", lpicture_size },
		{ "polygon_size", lpolygon_size },
		{ "pack_size", lpack_size },
		{ "animation_size", lanimation_size },
		{ "part_size", lpart_size },
		{ "string_size" , lstring_size },
		{ "import", limport },
		{ "dump", ldumppack },
		{ NULL, NULL },
	};

	luaL_newlib(L,l);

	lua_pushinteger(L, TYPE_PICTURE);
	lua_setfield(L, -2, "TYPE_PICTURE");
	lua_pushinteger(L, TYPE_ANIMATION);
	lua_setfield(L, -2, "TYPE_ANIMATION");
	lua_pushinteger(L, TYPE_POLYGON);
	lua_setfield(L, -2, "TYPE_POLYGON");

	return 1;
}