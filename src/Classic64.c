#ifdef _WIN32
	#define CC_API __declspec(dllimport)
	#define CC_VAR __declspec(dllimport)
	#define EXPORT __declspec(dllexport)
#else
	#define CC_API
	#define CC_VAR
	#define EXPORT __attribute__((visibility("default")))
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libsm64.h"
#include "decomp/include/sm64.h"
#include "decomp/include/surface_terrains.h"
#include "sha1/sha1.h"

#include "Classic64_events.h"
#include "Classic64_settings.h"
#include "Classic64.h"

#include "ClassiCube/Bitmap.h"
#include "ClassiCube/Block.h"
#include "ClassiCube/Camera.h"
#include "ClassiCube/Chat.h"
#include "ClassiCube/Drawer2D.h"
#include "ClassiCube/Entity.h"
#include "ClassiCube/Event.h"
#include "ClassiCube/ExtMath.h"
#include "ClassiCube/Game.h"
#include "ClassiCube/Graphics.h"
#include "ClassiCube/Gui.h"
#include "ClassiCube/Input.h"
#include "ClassiCube/Lighting.h"
#include "ClassiCube/Model.h"
#include "ClassiCube/Protocol.h"
#include "ClassiCube/Server.h"
#include "ClassiCube/Window.h"
#include "ClassiCube/World.h"

#define sign(x) ((x>0)?1:(x<0)?-1:0)

// shortcut to log messages to chat
static void SendChat(const char* format, const void* arg1, const void* arg2, const void* arg3, const void* arg4) {
	cc_string msg; char msgBuffer[256];
	String_InitArray(msg, msgBuffer);

	String_Format4(&msg, format, arg1, arg2, arg3, arg4);
	Chat_Add(&msg);
}

BlockID World_SafeGetBlock(int x, int y, int z) {
	return
		World_Contains(x, y, z) ? World_GetBlock(x, y, z) :
		(y < 0) ? BLOCK_BEDROCK :
		BLOCK_AIR;
}

float Math_LerpAngle(float leftAngle, float rightAngle, float t) {
	/* We have to cheat a bit for angles here */
	/* Consider 350* --> 0*, we only want to travel 10* */
	/* But without adjusting for this case, we would interpolate back the whole 350* degrees */
	cc_bool invertLeft  = leftAngle  > 270.0f && rightAngle < 90.0f;
	cc_bool invertRight = rightAngle > 270.0f && leftAngle  < 90.0f;
	if (invertLeft)  leftAngle  = leftAngle  - 360.0f;
	if (invertRight) rightAngle = rightAngle - 360.0f;

	return leftAngle + (rightAngle - leftAngle) * t;
}

// classicube symbols
static void LoadSymbolsFromGame(void);
static struct _ServerConnectionData* Server_;
static struct _BlockLists* Blocks_;
static struct _ModelsData* Models_;
static struct _GameData* Game_;
static struct _WorldData* World_;
static struct _EnvData* Env_;
static struct _EntitiesData* Entities_;
static struct _CameraData* Camera_;
static struct _GuiData* Gui_;
static struct _Lighting* Lighting_;
static struct _ChatEventsList* ChatEvents_;
static struct _GfxEventsList* GfxEvents_;
static struct _InputEventsList* InputEvents_;

bool isGuiOpen() {return Gui_->InputGrab;}

bool isBlockSolid(BlockID block)
{
	return (block != 0 && (Blocks_->Collide[block] == COLLIDE_SOLID || Blocks_->ExtendedCollide[block] == COLLIDE_LAVA));
}

// mario variables
uint8_t *marioTextureUint8;
struct MarioInstance *marioInstances[256];

int ticksBeforeSpawn;
bool inited;
bool allowTick; // false when loading world
float marioInterpTicks;

// mario's model (the hard part)
GfxResourceID marioTextureID;
struct Bitmap marioBitmap = {0, 1024, SM64_TEXTURE_HEIGHT};

static void marioModel_Draw(struct Entity* p)
{
	for (int i=0; i<256; i++)
	{
		if (Entities_->List[i] && Entities_->List[i] == p && marioInstances[i])
		{
			struct MarioInstance *obj = marioInstances[i];
			Gfx_BindTexture(marioTextureID);

			// draw colored triangles first (can't use VERTEX_FORMAT_COLOURED with VertexColoured structs because that crashes randomly)
			Gfx_BindVb(obj->vertexID);
			Gfx_DrawVb_IndexedTris(4 * obj->geometry.numTrianglesUsed);

			// draw textured triangles next (eyes, mustache, etc)
			Gfx_BindVb(obj->texturedVertexID);
			Gfx_DrawVb_IndexedTris(4 * obj->numTexturedTriangles);

#ifdef CLASSIC64_DEBUG
			if (pluginOptions[PLUGINOPTION_SURFACE_DEBUGGER].value.on)
			{
				Gfx_BindVb(obj->debuggerVertexID);
				Gfx_DrawVb_IndexedTris(obj->numDebuggerTriangles);
			}
#endif
			break;
		}
	}
}

static void marioModel_GetTransform(struct Entity* entity, Vec3 pos, struct Matrix* m)
{
	struct Matrix tmp;
	Matrix_Scale(m, 1,1,1);
	Matrix_Translate(&tmp, pos.X, pos.Y, pos.Z);
	Matrix_MulBy(m, &tmp);
}

static struct ModelTex unused_tex = { "char.png" }; // without this, the game crashes in first person view with nothing held in hand
static void marioModel_DrawArm(struct Entity* entity) {}
static void marioModel_MakeParts(void) {}
static float marioModel_GetNameY(struct Entity* e) { return 1+0.25f; }
static float marioModel_GetEyeY(struct Entity* e)  { return 1; }
static void marioModel_GetSize(struct Entity* e) {e->Size = (Vec3) { 0.5f, 1, 0.5f };}
static void marioModel_GetBounds(struct Entity* e) {AABB_Make(&e->ModelAABB, &e->Position, &e->Size);}
static struct Model mario_model = { "mario64", NULL, &unused_tex,
	marioModel_MakeParts, marioModel_Draw,
	marioModel_GetNameY,  marioModel_GetEyeY,
	marioModel_GetSize,   marioModel_GetBounds
};

static struct Model* marioModel_GetInstance(void) {
	Model_Init(&mario_model);
	mario_model.GetTransform = marioModel_GetTransform;
	mario_model.DrawArm = marioModel_DrawArm;
	mario_model.shadowScale = 0.85f;
	mario_model.usesHumanSkin = false;
	mario_model.bobbing = false;
	return &mario_model;
}

// chat command
void OnMarioClientCmd(const cc_string* args, int argsCount)
{
	cc_string empty = String_FromConst("");
	cc_string on = String_FromConst("on");
	cc_string off = String_FromConst("off");
	if (!argsCount || String_Compare(&args[0], &empty) == 0)
	{
		SendChat("&cSee /client help mario64 for available options", NULL, NULL, NULL, NULL);
		return;
	}

	cc_string options[] = {
		String_FromConst("music"),
		String_FromConst("cap"),
		String_FromConst("kill"),
		String_FromConst("force"),
		String_FromConst("settings"),
	};

	if (String_Compare(&args[0], &options[0]) == 0) // music
	{
		if (argsCount < 2)
		{
			SendChat("&a/client mario64 music <id 0-33>", NULL, NULL, NULL, NULL);
			SendChat("&ePlay music from Super Mario 64.", NULL, NULL, NULL, NULL);
			SendChat("&eTo stop music, use music ID 0.", NULL, NULL, NULL, NULL);
			return;
		}

		cc_uint8 music;
		if (!Convert_ParseUInt8(&args[1], &music) || music >= 0x22)
		{
			SendChat("&cInvalid music ID", NULL, NULL, NULL, NULL);
			return;
		}

		for (int i=0; i<0x22; i++) sm64_stop_background_music(i);
		if (music != 0) sm64_play_music(0, ((0 << 8) | music), 0);
		return;
	}
	else if (String_Compare(&args[0], &options[1]) == 0) // set mario cap setting
	{
		if (argsCount < 2)
		{
			SendChat("&a/client mario64 cap <option>", NULL, NULL, NULL, NULL);
			SendChat("&eChange Mario's cap.", NULL, NULL, NULL, NULL);
			SendChat("&eOptions: off, on, wing, metal", NULL, NULL, NULL, NULL);
			return;
		}

		if (!marioInstances[ENTITIES_SELF_ID])
		{
			SendChat("&cSwitch to Mario first", NULL, NULL, NULL, NULL);
			return;
		}

		struct {
			cc_string name;
			uint32_t capFlag;
		} caps[] = {
			{String_FromConst("off"), 0},
			{String_FromConst("on"), MARIO_NORMAL_CAP},
			{String_FromConst("wing"), MARIO_WING_CAP},
			{String_FromConst("metal"), MARIO_METAL_CAP}
		};

		for (int i=0; i<4; i++)
		{
			if (String_Compare(&args[1], &caps[i].name) == 0)
			{
				sm64_set_mario_state(marioInstances[ENTITIES_SELF_ID]->ID, 0);
				sm64_mario_interact_cap(marioInstances[ENTITIES_SELF_ID]->ID, caps[i].capFlag, 65535, 0);
				return;
			}
		}
		SendChat("&cUnknown cap \"%s\"", &args[1], NULL, NULL, NULL);
		return;
	}
	else if (String_Compare(&args[0], &options[2]) == 0) // kill mario
	{
		if (!marioInstances[ENTITIES_SELF_ID])
		{
			SendChat("&cSwitch to Mario first", NULL, NULL, NULL, NULL);
			return;
		}
		sm64_mario_kill(marioInstances[ENTITIES_SELF_ID]->ID);
		return;
	}
	else if (String_Compare(&args[0], &options[3]) == 0) // force-switch to mario
	{
		if (String_ContainsConst(&Server_->MOTD, "-hax") || String_ContainsConst(&Server_->MOTD, "-fly"))
		{
			SendChat("&cHacks are disabled, cannot switch to Mario", NULL, NULL, NULL, NULL);
			return;
		}
		cc_string model = String_FromConst( (strcmp(Entities_->List[ENTITIES_SELF_ID]->Model->name, "mario64") == 0) ? "human" : "mario64" );
		Entity_SetModel(Entities_->List[ENTITIES_SELF_ID], &model);
		return;
	}
	else if (String_Compare(&args[0], &options[4]) == 0) // change plugin settings
	{
		if (argsCount < 2)
		{
			SendChat("&a/client mario64 settings <option>", NULL, NULL, NULL, NULL);
			SendChat("&eChange plugin settings.", NULL, NULL, NULL, NULL);

			char msgBuffer[256];
			int printed = 0;
			sprintf(msgBuffer, "&eAvailable settings: ");
			for (int i=0; i<PLUGINOPTIONS_MAX; i++)
			{
				if (printed >= 5)
				{
					// multi lines
					printed = 0;
					SendChat(msgBuffer, NULL, NULL, NULL, NULL);
					sprintf(msgBuffer, "&e");
				}

				if (pluginOptions[i].hidden) continue;
				strcat(msgBuffer, pluginOptions[i].name.buffer);
				if (i != PLUGINOPTIONS_MAX-1) strcat(msgBuffer, ", ");
				printed++;
			}
			SendChat(msgBuffer, NULL, NULL, NULL, NULL);
			return;
		}

		for (int i=0; i<PLUGINOPTIONS_MAX; i++)
		{
			if (pluginOptions[i].hidden) continue;
			if (String_Compare(&args[1], &pluginOptions[i].name) == 0)
			{
				if (argsCount < 3)
				{
					SendChat("&a/client mario64 settings %s %s", &pluginOptions[i].name, &usageStrings[pluginOptions[i].valueType], NULL, NULL);
					for (int j=0; j<pluginOptions[i].descLines; j++) SendChat("%s", &pluginOptions[i].desc[j], NULL, NULL, NULL);

					cc_string value; char valueBuf[32];
					String_InitArray(value, valueBuf);
					switch(pluginOptions[i].valueType)
					{
						case PLUGINOPTION_VALUE_BOOL:
							String_AppendConst(&value, (pluginOptions[i].value.on) ? "on" : "off");
							break;
						case PLUGINOPTION_VALUE_NUMBER:
							String_AppendInt(&value, pluginOptions[i].value.num.current);
							break;
						case PLUGINOPTION_VALUE_STRING:
							String_AppendConst(&value, pluginOptions[i].value.str.current);
							break;
						case PLUGINOPTION_VALUE_RGB:
							{
								char colBuf[16];
								sprintf(colBuf, "%d %d %d", pluginOptions[i].value.col.r, pluginOptions[i].value.col.g, pluginOptions[i].value.col.b);
								String_AppendConst(&value, colBuf);
							}
							break;
					}

					SendChat("Current value: %s", &value, NULL, NULL, NULL);
					return;
				}

				switch(pluginOptions[i].valueType)
				{
					case PLUGINOPTION_VALUE_BOOL:
						if (String_Compare(&args[2], &on) == 0) pluginOptions[i].value.on = true;
						else if (String_Compare(&args[2], &off) == 0) pluginOptions[i].value.on = false;
						else
						{
							SendChat("&cUnknown value \"%s\". Please enter 'on' or 'off'.", &args[2], NULL, NULL, NULL);
							return;
						}

						SendChat("&a%s: %s", &pluginOptions[i].name, &args[2], NULL, NULL);
						break;

					case PLUGINOPTION_VALUE_NUMBER:
						{
							int val;
							if (!Convert_ParseInt(&args[2], &val))
							{
								SendChat("&cUnknown value \"%s\". Please enter a number.", &args[2], NULL, NULL, NULL);
								return;
							}
							if (val < pluginOptions[i].value.num.min || val > pluginOptions[i].value.num.max)
							{
								cc_string Min; char MinBuf[32];
								cc_string Max; char MaxBuf[32];
								String_InitArray(Min, MinBuf);
								String_InitArray(Max, MaxBuf);
								String_AppendInt(&Min, pluginOptions[i].value.num.min);
								String_AppendInt(&Max, pluginOptions[i].value.num.max);

								SendChat("&cNumber \"%s\" out of range. Please enter a number between %s and %s.", &args[2], &Min, &Max, NULL);
								return;
							}

							pluginOptions[i].value.num.current = val;
							SendChat("&a%s: %s", &pluginOptions[i].name, &args[2], NULL, NULL);
						}
						break;

					case PLUGINOPTION_VALUE_STRING:
						{
							if (pluginOptions[i].value.str.validList)
							{
								// user must pick a value from a list

								bool found = false;
								for (int j=0; j<pluginOptions[i].value.str.validMax && !found; j++)
								{
									if (strcmp(args[2].buffer, pluginOptions[i].value.str.validList[j]) == 0)
										found = true;
								}

								if (!found)
								{
									char allBuf[512];
									sprintf(allBuf, "&4");
									int w = 0;

									SendChat("&cUnknown value \"%s\". Please enter one of these values:", &args[2], NULL, NULL, NULL);
									for (int j=0; j<pluginOptions[i].value.str.validMax; j++)
									{
										if (w > 64)
										{
											// multi-line
											SendChat(allBuf, NULL, NULL, NULL, NULL);
											sprintf(allBuf, "&4");
											w = 0;
										}

										strcat(allBuf, pluginOptions[i].value.str.validList[j]);
										w += strlen(pluginOptions[i].value.str.validList[j]);
										if (j != pluginOptions[i].value.str.validMax-1)
										{
											strcat(allBuf, ", ");
											w += 2;
										}
									}
									SendChat(allBuf, NULL, NULL, NULL, NULL);
									return;
								}
							}

							strcpy(pluginOptions[i].value.str.current, args[2].buffer);
							SendChat("&a%s: %s", &pluginOptions[i].name, &args[2], NULL, NULL);
						}
						break;

					case PLUGINOPTION_VALUE_RGB:
						if (argsCount < 5)
						{
							SendChat("&cNot enough arguments. Please enter RGB values between 0 and 255.", NULL, NULL, NULL, NULL);
							return;
						}

						if (!Convert_ParseUInt8(&args[2], &pluginOptions[i].value.col.r) || !Convert_ParseUInt8(&args[3], &pluginOptions[i].value.col.g) || !Convert_ParseUInt8(&args[4], &pluginOptions[i].value.col.b))
						{
							SendChat("&cParse failed. Please enter RGB number values between 0 and 255.", NULL, NULL, NULL, NULL);
							return;
						}

						SendChat("&a%s: %s, %s, %s", &pluginOptions[i].name, &args[2], &args[3], &args[4]);
						break;
				}

				saveSettings();
				return;
			}
		}

		SendChat("&cUnknown setting \"%s\"", &args[1], NULL, NULL, NULL);
		return;
	}

	SendChat("&cUnknown option \"%s\"", &args[0], NULL, NULL, NULL);
}

static struct ChatCommand MarioClientCmd = {
	"Mario64", OnMarioClientCmd, false,
	{
		"&a/client mario64 <option>",
		"&eAvailable options:",
		"&emusic, cap, kill, force, settings",
	}
};

// mario functions
void deleteBlocks(uint32_t *arrayTarget) // specify arrayTarget because the blocks need to be created before mario can be spawned
{
	for (int j=0; j<128; j++)
	{
		if (arrayTarget[j] == -1) continue;
		sm64_surface_object_delete(arrayTarget[j]);
		arrayTarget[j] = -1;
	}
}

void deleteMario(int i)
{
	if (!marioInstances[i]) return;
	struct MarioInstance *obj = marioInstances[i];
	printf("delete mario %d\n", i);

	deleteBlocks(obj->surfaces);
	sm64_mario_delete(obj->ID);

	free(obj->geometry.position);
	free(obj->geometry.color);
	free(obj->geometry.normal);
	free(obj->geometry.uv);
	Gfx_DeleteDynamicVb(&obj->vertexID);
	Gfx_DeleteDynamicVb(&obj->texturedVertexID);
#ifdef CLASSIC64_DEBUG
	Gfx_DeleteDynamicVb(&obj->debuggerVertexID);
#endif

	// restore the original VTABLE if the entity still exists
	if (Entities_->List[i]) Entities_->List[i]->VTABLE = marioInstances[i]->OriginalVTABLE;

	free(marioInstances[i]);
	marioInstances[i] = 0;
}

bool addBlock(int x, int y, int z, int *i, uint32_t *arrayTarget, bool* moreBelow)
{
	BlockID block = World_SafeGetBlock(x, y, z);
	if (!isBlockSolid(block)) return false;

	struct SM64SurfaceObject obj;
	memset(&obj.transform, 0, sizeof(struct SM64ObjectTransform));
	obj.transform.position[0] = x * IMARIO_SCALE;
	obj.transform.position[1] = y * IMARIO_SCALE;
	obj.transform.position[2] = z * IMARIO_SCALE;
	obj.surfaceCount = 0;
	obj.surfaces = (struct SM64Surface*)malloc(sizeof(struct SM64Surface) * 6*2);

	Vec3 *Min = &Blocks_->MinBB[block];
	Vec3 *Max = &Blocks_->MaxBB[block];

	BlockID up =		World_SafeGetBlock(x, y+1, z);
	BlockID down =		World_SafeGetBlock(x, y-1, z);
	BlockID left =		World_SafeGetBlock(x, y, z+1);
	BlockID right =		World_SafeGetBlock(x, y, z-1);
	BlockID back =		World_SafeGetBlock(x+1, y, z);
	BlockID front =		World_SafeGetBlock(x-1, y, z);

	// block ground face
	if (!up || !isBlockSolid(up) || Blocks_->MinBB[up].Y != 0 || Blocks_->MinBB[up].X != 0 || Blocks_->MaxBB[up].X != 1 || Blocks_->MinBB[up].Z != 0 || Blocks_->MaxBB[up].Z != 1)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][2] = Max->Z * IMARIO_SCALE;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = Min->X * IMARIO_SCALE; 	obj.surfaces[obj.surfaceCount+1].vertices[0][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][2] = Min->Z * IMARIO_SCALE;

		obj.surfaceCount += 2;
	}

	// left (Z+)
	if (!left || !isBlockSolid(left) || Blocks_->MinBB[left].Z != 0 || Blocks_->MinBB[left].X != 0 || Blocks_->MaxBB[left].X != 1 || Blocks_->MinBB[left].Y != 0 || Blocks_->MaxBB[left].Y != 1)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][2] = Max->Z * IMARIO_SCALE;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][2] = Max->Z * IMARIO_SCALE;

		obj.surfaceCount += 2;
	}

	// right (Z-)
	if (!right || !isBlockSolid(right) || Blocks_->MaxBB[right].Z != 1 || Blocks_->MinBB[right].X != 0 || Blocks_->MaxBB[right].X != 1 || Blocks_->MinBB[right].Y != 0 || Blocks_->MaxBB[right].Y != 1)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][2] = Min->Z * IMARIO_SCALE;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][2] = Min->Z * IMARIO_SCALE;

		obj.surfaceCount += 2;
	}

	// back (X+)
	if (!back || !isBlockSolid(back) || Blocks_->MinBB[back].X != 0 || Blocks_->MinBB[back].Y != 0 || Blocks_->MaxBB[back].Y != 1 || Blocks_->MinBB[back].Z != 0 || Blocks_->MaxBB[back].Z != 1)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][2] = Max->Z * IMARIO_SCALE;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][2] = Min->Z * IMARIO_SCALE;

		obj.surfaceCount += 2;
	}

	// front (X-)
	if (!front || !isBlockSolid(front) || Blocks_->MaxBB[back].X != 1 || Blocks_->MinBB[front].Y != 0 || Blocks_->MaxBB[front].Y != 1 || Blocks_->MinBB[front].Z != 0 || Blocks_->MaxBB[front].Z != 1)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][2] = Min->Z * IMARIO_SCALE;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][1] = Max->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][2] = Max->Z * IMARIO_SCALE;

		obj.surfaceCount += 2;
	}

	// block bottom face
	if (!down || !isBlockSolid(down) || Blocks_->MaxBB[down].Y != 1 || Blocks_->MinBB[down].X != 0 || Blocks_->MaxBB[down].X != 1 || Blocks_->MinBB[down].Z != 0 || Blocks_->MaxBB[down].Z != 1)
	{
		obj.surfaces[obj.surfaceCount+0].vertices[0][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[0][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[1][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[1][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+0].vertices[2][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+0].vertices[2][2] = Max->Z * IMARIO_SCALE;

		obj.surfaces[obj.surfaceCount+1].vertices[0][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[0][2] = Min->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[1][0] = Max->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[1][2] = Max->Z * IMARIO_SCALE;
		obj.surfaces[obj.surfaceCount+1].vertices[2][0] = Min->X * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][1] = Min->Y * IMARIO_SCALE;	obj.surfaces[obj.surfaceCount+1].vertices[2][2] = Min->Z * IMARIO_SCALE;

		obj.surfaceCount += 2;
	}

	for (int ind=0; ind<obj.surfaceCount; ind++)
	{
		obj.surfaces[ind].type =
			(Blocks_->ExtendedCollide[block] == COLLIDE_ICE || Blocks_->ExtendedCollide[block] == COLLIDE_SLIPPERY_ICE) ? SURFACE_ICE :
			(Blocks_->ExtendedCollide[block] == COLLIDE_LAVA) ? SURFACE_BURNING :
			SURFACE_DEFAULT;
		obj.surfaces[ind].force = 0;
		if (obj.surfaces[ind].type == SURFACE_ICE)
		{
			obj.surfaces[ind].type = SURFACE_VERY_SLIPPERY;
			obj.surfaces[ind].terrain = TERRAIN_WATER;
		}
		else switch(Blocks_->StepSounds[block])
		{
			case SOUND_SNOW:
				obj.surfaces[ind].terrain = TERRAIN_SNOW;
				break;
			case SOUND_CLOTH:
			case SOUND_METAL:
				obj.surfaces[ind].terrain = TERRAIN_GRASS;
				break;
			case SOUND_GRASS:
			case SOUND_GRAVEL:
				obj.surfaces[ind].type = SURFACE_NOISE_DEFAULT;
				obj.surfaces[ind].terrain = TERRAIN_GRASS;
				break;
			case SOUND_SAND:
				obj.surfaces[ind].terrain = TERRAIN_SAND;
				break;
			case SOUND_WOOD:
				obj.surfaces[ind].terrain = TERRAIN_SPOOKY;
				break;
			default:
				obj.surfaces[ind].terrain = TERRAIN_STONE;
				break;
		}
	}

	if (obj.surfaceCount)
		arrayTarget[(*i)++] = sm64_surface_object_create(&obj);

	if (moreBelow) *moreBelow = (Min->X != 0 || Max->X != 1 || Min->Z != 0 || Max->Z != 1);
	free(obj.surfaces);
	return true;
}

void loadNewBlocks(int i, int x, int y, int z, uint32_t *arrayTarget) // specify arrayTarget because the blocks need to be created before mario can be spawned
{
	deleteBlocks(arrayTarget);
	int yadd = 0;

	int arrayInd = 0;
	for (int zadd=-1; zadd<=1; zadd++)
	{
		for (int xadd=-1; xadd<=1; xadd++)
		{
			if (x+xadd < 0 || x+xadd >= World_->Width || z+zadd < 0 || z+zadd >= World_->Length) continue;

			for (yadd=2; yadd>=0; yadd--)
			{
				if (y+yadd < World_->Height)
					addBlock(x+xadd, y+yadd, z+zadd, &arrayInd, arrayTarget, NULL);
			}

			// get block at floor
			for (yadd=-1; y+yadd>=-1; yadd--)
			{
				bool moreBelow = false;
				if (addBlock(x+xadd, y+yadd, z+zadd, &arrayInd, arrayTarget, &moreBelow) && !moreBelow) break;
			}

			// rope interaction
			if (xadd == 0 && zadd == 0)
			{
				BlockID rope = World_SafeGetBlock(x, y, z);
				if (Blocks_->Collide[rope] == COLLIDE_CLIMB_ROPE)
				{
					int height = 0, bottom = 0;
					while (Blocks_->Collide[World_SafeGetBlock(x, y-bottom-1, z)] == COLLIDE_CLIMB_ROPE)
						bottom++;
					while (Blocks_->Collide[World_SafeGetBlock(x, y-bottom+height+1, z)] == COLLIDE_CLIMB_ROPE)
						height++;

					if (marioInstances[i])
					{
						struct MarioInstance* obj = marioInstances[i];
						if (!(obj->state.action & ACT_FLAG_ON_POLE))
						{
							sm64_set_mario_pole(obj->ID, (x*MARIO_SCALE+(MARIO_SCALE/2)), (y-bottom)*MARIO_SCALE, (z*MARIO_SCALE+(MARIO_SCALE/2)), height*MARIO_SCALE);

							float xdist = fabs((x*MARIO_SCALE+(MARIO_SCALE/2)) - obj->currPos.X);
							float zdist = fabs((z*MARIO_SCALE+(MARIO_SCALE/2)) - obj->currPos.Z);
							if ((obj->state.action & ACT_FLAG_AIR) && (obj->lastPoleHeight != height || obj->lastPole.X != x || obj->lastPole.Y != y-bottom || obj->lastPole.Z != z) && xdist < 40 && zdist < 40)
							{
								obj->lastPole = (Vec3){x, y-bottom, z};
								obj->lastPoleHeight = height;
								sm64_set_mario_action(obj->ID, ACT_GRAB_POLE_SLOW);
							}
						}
					}
				}
				else if (marioInstances[i])
				{
					marioInstances[i]->lastPole = (Vec3){0};
					marioInstances[i]->lastPoleHeight = 0;
				}
			}
		}
	}

#ifdef CLASSIC64_DEBUG
	if (marioInstances[i])
	{
		struct MarioInstance* mario = marioInstances[i];
		uint32_t objCount = 0, vCount = 0;
		struct LoadedSurfaceObject* objs = sm64_get_all_surface_objects(&objCount);

		float u[3] = {0.95, 0.96, 0.96};
		float v[3] = {0, 0, 1};
		bool bgr = (pluginOptions[PLUGINOPTION_BGR].value.on);

		for (uint32_t j=0; j<objCount; j++)
		{
			for (int k=0; k<objs[j].surfaceCount; k++)
			{
				for (int l=0; l<3; l++)
				{
					mario->debuggerVertices[vCount+l] = (struct VertexTextured) {
						(objs[j].transform->aPosX - (mario->state.position[0]) + objs[j].libSurfaces[k].vertices[l][0]) / MARIO_SCALE,
						(objs[j].transform->aPosY - (mario->state.position[1]) + objs[j].libSurfaces[k].vertices[l][1]) / MARIO_SCALE,
						(objs[j].transform->aPosZ - (mario->state.position[2]) + objs[j].libSurfaces[k].vertices[l][2]) / MARIO_SCALE,
						PackedCol_Make(0, 255, 0, 255), u[l], v[l]
					};
				}

				if (mario->debuggerVertices[vCount+0].X == mario->debuggerVertices[vCount+1].X &&
				    mario->debuggerVertices[vCount+1].X == mario->debuggerVertices[vCount+2].X &&
					mario->debuggerVertices[vCount+0].X == mario->debuggerVertices[vCount+2].X) // X is red
				{
					mario->debuggerVertices[vCount].Col = mario->debuggerVertices[vCount+1].Col = mario->debuggerVertices[vCount+2].Col = PackedCol_Make(bgr?0:255, 0, bgr?255:0, 255);
				}
				else if (mario->debuggerVertices[vCount+0].Y == mario->debuggerVertices[vCount+1].Y &&
				         mario->debuggerVertices[vCount+1].Y == mario->debuggerVertices[vCount+2].Y &&
					     mario->debuggerVertices[vCount+0].Y == mario->debuggerVertices[vCount+2].Y) // Y is green
				{
					mario->debuggerVertices[vCount].Col = mario->debuggerVertices[vCount+1].Col = mario->debuggerVertices[vCount+2].Col = PackedCol_Make(0, 255, 0, 255);
				}
				else if (mario->debuggerVertices[vCount+0].Z == mario->debuggerVertices[vCount+1].Z &&
				         mario->debuggerVertices[vCount+1].Z == mario->debuggerVertices[vCount+2].Z &&
					     mario->debuggerVertices[vCount+0].Z == mario->debuggerVertices[vCount+2].Z) // Y is green
				{
					mario->debuggerVertices[vCount].Col = mario->debuggerVertices[vCount+1].Col = mario->debuggerVertices[vCount+2].Col = PackedCol_Make(bgr?255:0, 0, bgr?0:255, 255);
				}

				mario->debuggerVertices[vCount+3] = mario->debuggerVertices[vCount+2];
				vCount += 4;
			}
		}
		mario->numDebuggerTriangles = vCount;
		Gfx_SetDynamicVbData(mario->debuggerVertexID, &mario->debuggerVertices, DEBUGGER_MAX_VERTICES);
	}
#endif
}

// custom callback for mario VTABLE
void marioSetLocation(struct Entity* e, struct LocationUpdate* update, cc_bool interpolate)
{
	for (int i=0; i<256; i++)
	{
		if (Entities_->List[i] && Entities_->List[i] == e && marioInstances[i])
		{
			marioInstances[i]->OriginalVTABLE->SetLocation(e, update, interpolate);

			if (update->Flags & LOCATIONUPDATE_POS)
				sm64_set_mario_position(marioInstances[i]->ID, update->Pos.X*MARIO_SCALE, update->Pos.Y*MARIO_SCALE, update->Pos.Z*MARIO_SCALE);

			break;
		}
	}
}

void marioTick(struct ScheduledTask* task)
{
	if (!allowTick) return;

	for (int i=0; i<256; i++)
	{
		if (!Entities_->List[i]) continue;
		if (!marioInstances[i] && ticksBeforeSpawn <= 0 && strcmp(Entities_->List[i]->Model->name, "mario64") == 0)
		{
			// spawn mario. have some temporary variables for the surface IDs and mario ID
			uint32_t surfaces[128];
			memset(surfaces, -1, sizeof(surfaces));
			loadNewBlocks(i, Entities_->List[i]->Position.X, Entities_->List[i]->Position.Y, Entities_->List[i]->Position.Z, surfaces);
			int32_t ID = sm64_mario_create(Entities_->List[i]->Position.X*IMARIO_SCALE, Entities_->List[i]->Position.Y*IMARIO_SCALE, Entities_->List[i]->Position.Z*IMARIO_SCALE, 0,0,0,0);
			if (ID == -1)
			{
				SendChat("&cFailed to spawn Mario", NULL, NULL, NULL, NULL);
			}
			else
			{
				// mario was spawned, intialize the instance struct and put everything in there
				sm64_set_mario_faceangle(ID, (int16_t)((-Entities_->List[i]->Yaw + 180) / 180 * 32768.0f));
				marioInstances[i] = (struct MarioInstance*)malloc(sizeof(struct MarioInstance));
				marioInstances[i]->ID = ID;
				memcpy(&marioInstances[i]->surfaces, surfaces, sizeof(surfaces));
				marioInstances[i]->lastPos.X = marioInstances[i]->currPos.X = Entities_->List[i]->Position.X*IMARIO_SCALE;
				marioInstances[i]->lastPos.Y = marioInstances[i]->currPos.Y = Entities_->List[i]->Position.Y*IMARIO_SCALE;
				marioInstances[i]->lastPos.Z = marioInstances[i]->currPos.Z = Entities_->List[i]->Position.Z*IMARIO_SCALE;
				memset(&marioInstances[i]->input, 0, sizeof(struct SM64MarioInputs));
				marioInstances[i]->geometry.position = malloc( sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES );
				marioInstances[i]->geometry.color    = malloc( sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES );
				marioInstances[i]->geometry.normal   = malloc( sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES );
				marioInstances[i]->geometry.uv       = malloc( sizeof(float) * 6 * SM64_GEO_MAX_TRIANGLES );
				marioInstances[i]->vertexID = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4 * SM64_GEO_MAX_TRIANGLES);
				marioInstances[i]->texturedVertexID = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4 * SM64_GEO_MAX_TRIANGLES);
				marioInstances[i]->numTexturedTriangles = 0;
				marioInstances[i]->lastPole = (Vec3){0};
				marioInstances[i]->lastPoleHeight = 0;
#ifdef CLASSIC64_DEBUG
				marioInstances[i]->debuggerVertexID = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, DEBUGGER_MAX_VERTICES);
#endif

				// backup the original entity VTABLE.
				marioInstances[i]->OriginalVTABLE = Entities_->List[i]->VTABLE;

				// replace this entity's VTABLE with one suited for mario.
				// if the player walks into a portal or uses a teleport command, it'll set mario's position as well
				// (sorry Unk, but it had to be done)
				marioInstances[i]->marioVTABLE = (const struct EntityVTABLE){
					Entities_->List[i]->VTABLE->Tick, Entities_->List[i]->VTABLE->Despawn, marioSetLocation,
					Entities_->List[i]->VTABLE->GetCol, Entities_->List[i]->VTABLE->RenderModel, Entities_->List[i]->VTABLE->RenderName
				};
				Entities_->List[i]->VTABLE = &marioInstances[i]->marioVTABLE;
			}
		}
		else if (marioInstances[i])
		{
			struct MarioInstance *obj = marioInstances[i];
			if (strcmp(Entities_->List[i]->Model->name, "mario64"))
			{
				// no longer mario, delete from memory
				deleteMario(i);
				continue;
			}

			if (i != ENTITIES_SELF_ID) // not you
			{
				Vec3 newPos = {Entities_->List[i]->Position.X*IMARIO_SCALE, Entities_->List[i]->Position.Y*IMARIO_SCALE, Entities_->List[i]->Position.Z*IMARIO_SCALE};
				sm64_set_mario_position(obj->ID, newPos.X, newPos.Y, newPos.Z);
				obj->input.buttonA = (newPos.Y - obj->lastPos.Y > 0);

				bool moved = (newPos.Z - obj->lastPos.Z) || (newPos.X - obj->lastPos.X);
				if (moved)
				{
					float dir = atan2(newPos.Z - obj->lastPos.Z, newPos.X - obj->lastPos.X);
					obj->input.stickX = -Math_Cos(dir);
					obj->input.stickY = -Math_Sin(dir);
				}
				else
					obj->input.stickX = obj->input.stickY = 0;

				obj->lastPos = newPos;
			}
			else
			{
				marioInterpTicks = 0;
				obj->lastPos.X = obj->state.position[0]; obj->lastPos.Y = obj->state.position[1]; obj->lastPos.Z = obj->state.position[2];
				if (pluginOptions[PLUGINOPTION_INTERP].value.on)
				{
					for (int j=0; j<3 * SM64_GEO_MAX_TRIANGLES; j++)
					{
						obj->lastGeom[j] = obj->currGeom[j];
						obj->lastTexturedGeom[j] = obj->currTexturedGeom[j];
					}
				}
			}

			if (!pluginOptions[PLUGINOPTION_HURT].value.on && sm64_mario_get_health(obj->ID) != 0xff) sm64_mario_set_health(obj->ID, 0x880);

			// water
			int yadd = 0;
			int waterY = -1024;
			while (obj->state.position[1]/MARIO_SCALE+yadd > waterY && Blocks_->IsLiquid[World_SafeGetBlock(obj->state.position[0]/MARIO_SCALE, obj->state.position[1]/MARIO_SCALE+yadd, obj->state.position[2]/MARIO_SCALE)])
			{
				waterY = obj->state.position[1]/MARIO_SCALE+yadd;
				yadd++;
			}
			sm64_set_mario_water_level(obj->ID, waterY*IMARIO_SCALE+IMARIO_SCALE);

			sm64_mario_tick(obj->ID, &obj->input, &obj->state, &obj->geometry);
			obj->currPos.X = obj->state.position[0];
			obj->currPos.Y = obj->state.position[1];
			obj->currPos.Z = obj->state.position[2];

			obj->numTexturedTriangles = 0;
			bool bgr = (pluginOptions[PLUGINOPTION_BGR].value.on);
			bool customColors = (pluginOptions[PLUGINOPTION_CUSTOM_COLORS].value.on);

			// fix for 1.3.2 until a future release comes out with the lighting system refactor
			PackedCol lightCol = (Lighting_) ? Lighting_->Color(obj->state.position[0]/MARIO_SCALE, obj->state.position[1]/MARIO_SCALE, obj->state.position[2]/MARIO_SCALE) : Env_->SunCol;

			for (int j=0; j<obj->geometry.numTrianglesUsed; j++)
			{
				bool hasTexture = (obj->geometry.uv[j*6+0] != 1 && obj->geometry.uv[j*6+1] != 1 && obj->geometry.uv[j*6+2] != 1 && obj->geometry.uv[j*6+3] != 1 && obj->geometry.uv[j*6+4] != 1 && obj->geometry.uv[j*6+5] != 1);
				bool wingCap = (obj->state.flags & MARIO_WING_CAP && j >= obj->geometry.numTrianglesUsed-8 && j <= obj->geometry.numTrianglesUsed-1); // do not draw white rectangles around wingcap

				uint8_t r = obj->geometry.color[j*9+0]*255;
				uint8_t g = obj->geometry.color[j*9+1]*255;
				uint8_t b = obj->geometry.color[j*9+2]*255;
				if (customColors && i == ENTITIES_SELF_ID) // only give yourself custom colors
				{
					for (int c = 0; c < 6; c++)
					{
						if (r == defaultColors[c].r && g == defaultColors[c].g && b == defaultColors[c].b)
						{
							r = pluginOptions[PLUGINOPTION_COLOR_OVERALLS + c].value.col.r;
							g = pluginOptions[PLUGINOPTION_COLOR_OVERALLS + c].value.col.g;
							b = pluginOptions[PLUGINOPTION_COLOR_OVERALLS + c].value.col.b;
							break;
						}
					}
				}

				PackedCol col = PackedCol_Tint(PackedCol_Make((bgr)?b:r, g, (bgr)?r:b, 255), lightCol);

				obj->vertices[j*4+0] = (struct VertexTextured) {
					(obj->geometry.position[j*9+0] - obj->state.position[0]) / MARIO_SCALE,
					(obj->geometry.position[j*9+1] - obj->state.position[1]) / MARIO_SCALE,
					(obj->geometry.position[j*9+2] - obj->state.position[2]) / MARIO_SCALE,
					col,
					(wingCap) ? 0 : 0.95, (wingCap) ? 0.95 : 0
				};

				obj->vertices[j*4+1] = (struct VertexTextured) {
					(obj->geometry.position[j*9+3] - obj->state.position[0]) / MARIO_SCALE,
					(obj->geometry.position[j*9+4] - obj->state.position[1]) / MARIO_SCALE,
					(obj->geometry.position[j*9+5] - obj->state.position[2]) / MARIO_SCALE,
					col,
					(wingCap) ? 0 : 0.96, (wingCap) ? 0.96 : 0
				};

				obj->vertices[j*4+2] = (struct VertexTextured) {
					(obj->geometry.position[j*9+6] - obj->state.position[0]) / MARIO_SCALE,
					(obj->geometry.position[j*9+7] - obj->state.position[1]) / MARIO_SCALE,
					(obj->geometry.position[j*9+8] - obj->state.position[2]) / MARIO_SCALE,
					col,
					(wingCap) ? 0.01 : 0.96, (wingCap) ? 0.96 : 1
				};

				obj->vertices[j*4+3] = obj->vertices[j*4+2]; // WHY IS IT A QUAD?!?!?!?!? why does classicube use quads!??!?!??

				if (hasTexture)
				{
					for (int k=0; k<4; k++)
					{
						obj->texturedVertices[obj->numTexturedTriangles*4+k] = (struct VertexTextured) {
							obj->vertices[j*4+k].X, obj->vertices[j*4+k].Y, obj->vertices[j*4+k].Z,
							PackedCol_Tint(PACKEDCOL_WHITE, lightCol),
							obj->geometry.uv[j*6+(k*2+0)], obj->geometry.uv[j*6+(k*2+1)]
						};

						if (k<3 && i == ENTITIES_SELF_ID) // interpolation
							obj->currTexturedGeom[obj->numTexturedTriangles*3+k] = (Vec3){obj->texturedVertices[obj->numTexturedTriangles*4+k].X, obj->texturedVertices[obj->numTexturedTriangles*4+k].Y, obj->texturedVertices[obj->numTexturedTriangles*4+k].Z};
					}

					obj->numTexturedTriangles++;
				}

				if (i == ENTITIES_SELF_ID) // interpolation
				{
					for (int k=0; k<3; k++)
						obj->currGeom[j*3+k] = (Vec3){obj->vertices[j*4+k].X, obj->vertices[j*4+k].Y, obj->vertices[j*4+k].Z};
				}
			}
			Gfx_SetDynamicVbData(obj->vertexID, &obj->vertices, 4 * SM64_GEO_MAX_TRIANGLES);
			Gfx_SetDynamicVbData(obj->texturedVertexID, &obj->texturedVertices, 4 * SM64_GEO_MAX_TRIANGLES);

			//if ((int)(obj->lastPos.X) != (int)(obj->currPos.X) || (int)(obj->lastPos.Y) != (int)(obj->currPos.Y) || (int)(obj->lastPos.Z) != (int)(obj->currPos.Z))
			//{
				loadNewBlocks(i, obj->currPos.X/MARIO_SCALE, obj->currPos.Y/MARIO_SCALE, obj->currPos.Z/MARIO_SCALE, obj->surfaces);
			//}
		}
	}

	if (ticksBeforeSpawn > 0) ticksBeforeSpawn--;
}

void selfMarioTick(struct ScheduledTask* task)
{
	if (!marioInstances[ENTITIES_SELF_ID]) return;
	struct MarioInstance *obj = marioInstances[ENTITIES_SELF_ID];

	if (pluginOptions[PLUGINOPTION_INTERP].value.on)
	{
		// interpolate position and mesh
		obj->state.position[0] = obj->lastPos.X + ((obj->currPos.X - obj->lastPos.X) * (marioInterpTicks / (1.f/30)));
		obj->state.position[1] = obj->lastPos.Y + ((obj->currPos.Y - obj->lastPos.Y) * (marioInterpTicks / (1.f/30)));
		obj->state.position[2] = obj->lastPos.Z + ((obj->currPos.Z - obj->lastPos.Z) * (marioInterpTicks / (1.f/30)));

		for (int i=0; i<obj->geometry.numTrianglesUsed; i++)
		{
			for (int j=0; j<3; j++)
			{
				obj->vertices[i*4+j].X = obj->lastGeom[i*3+j].X + ((obj->currGeom[i*3+j].X - obj->lastGeom[i*3+j].X) * (marioInterpTicks / (1.f/30)));
				obj->vertices[i*4+j].Y = obj->lastGeom[i*3+j].Y + ((obj->currGeom[i*3+j].Y - obj->lastGeom[i*3+j].Y) * (marioInterpTicks / (1.f/30)));
				obj->vertices[i*4+j].Z = obj->lastGeom[i*3+j].Z + ((obj->currGeom[i*3+j].Z - obj->lastGeom[i*3+j].Z) * (marioInterpTicks / (1.f/30)));

				obj->texturedVertices[i*4+j].X = obj->lastTexturedGeom[i*3+j].X + ((obj->currTexturedGeom[i*3+j].X - obj->lastTexturedGeom[i*3+j].X) * (marioInterpTicks / (1.f/30)));
				obj->texturedVertices[i*4+j].Y = obj->lastTexturedGeom[i*3+j].Y + ((obj->currTexturedGeom[i*3+j].Y - obj->lastTexturedGeom[i*3+j].Y) * (marioInterpTicks / (1.f/30)));
				obj->texturedVertices[i*4+j].Z = obj->lastTexturedGeom[i*3+j].Z + ((obj->currTexturedGeom[i*3+j].Z - obj->lastTexturedGeom[i*3+j].Z) * (marioInterpTicks / (1.f/30)));
			}

			obj->vertices[i*4+3].X = obj->vertices[i*4+2].X;
			obj->vertices[i*4+3].Y = obj->vertices[i*4+2].Y;
			obj->vertices[i*4+3].Z = obj->vertices[i*4+2].Z;

			obj->texturedVertices[i*4+3].X = obj->texturedVertices[i*4+2].X;
			obj->texturedVertices[i*4+3].Y = obj->texturedVertices[i*4+2].Y;
			obj->texturedVertices[i*4+3].Z = obj->texturedVertices[i*4+2].Z;
		}
		Gfx_SetDynamicVbData(obj->vertexID, &obj->vertices, 4 * SM64_GEO_MAX_TRIANGLES);
		Gfx_SetDynamicVbData(obj->texturedVertexID, &obj->texturedVertices, 4 * SM64_GEO_MAX_TRIANGLES);

		if (marioInterpTicks < 1.f/30) marioInterpTicks += 1.f/300;
	}
	else
	{
		obj->state.position[0] = obj->currPos.X;
		obj->state.position[1] = obj->currPos.Y;
		obj->state.position[2] = obj->currPos.Z;
	}

	struct LocationUpdate update = {0};
	update.Flags = LOCATIONUPDATE_POS;
	update.Pos.X = obj->state.position[0] / MARIO_SCALE;
	update.Pos.Y = obj->state.position[1] / MARIO_SCALE;
	update.Pos.Z = obj->state.position[2] / MARIO_SCALE;
	update.RelativePos = false;
	if (pluginOptions[PLUGINOPTION_CAMERA].value.on)
	{
		update.Flags |= LOCATIONUPDATE_YAW;
		update.Yaw = Math_LerpAngle(Entities_->List[ENTITIES_SELF_ID]->Yaw, ((-obj->state.faceAngle + MATH_PI) * MATH_RAD2DEG), 0.03f);
	}
	obj->OriginalVTABLE->SetLocation(Entities_->List[ENTITIES_SELF_ID], &update, false);
	Entities_->List[ENTITIES_SELF_ID]->Velocity.X = Entities_->List[ENTITIES_SELF_ID]->Velocity.Y = Entities_->List[ENTITIES_SELF_ID]->Velocity.Z = 0;

	if (Gui_->InputGrab) // menu is open
	{
		memset(&obj->input, 0, sizeof(struct SM64MarioInputs));
		return;
	}

	float dir = 0;
	float spd = 0;
	if (KeyBind_IsPressed(KEYBIND_FORWARD) && KeyBind_IsPressed(KEYBIND_RIGHT))
	{
		dir = -MATH_PI * 0.25f;
		spd = 1;
	}
	else if (KeyBind_IsPressed(KEYBIND_FORWARD) && KeyBind_IsPressed(KEYBIND_LEFT))
	{
		dir = -MATH_PI * 0.75f;
		spd = 1;
	}
	else if (KeyBind_IsPressed(KEYBIND_BACK) && KeyBind_IsPressed(KEYBIND_RIGHT))
	{
		dir = MATH_PI * 0.25f;
		spd = 1;
	}
	else if (KeyBind_IsPressed(KEYBIND_BACK) && KeyBind_IsPressed(KEYBIND_LEFT))
	{
		dir = MATH_PI * 0.75f;
		spd = 1;
	}
	else if (KeyBind_IsPressed(KEYBIND_FORWARD))
	{
		dir = -MATH_PI * 0.5f;
		spd = 1;
	}
	else if (KeyBind_IsPressed(KEYBIND_BACK))
	{
		dir = MATH_PI * 0.5f;
		spd = 1;
	}
	else if (KeyBind_IsPressed(KEYBIND_LEFT))
	{
		dir = MATH_PI;
		spd = 1;
	}
	else if (KeyBind_IsPressed(KEYBIND_RIGHT))
	{
		dir = 0;
		spd = 1;
	}

	obj->input.stickX = Math_Cos(dir) * spd;
	obj->input.stickY = Math_Sin(dir) * spd;
	if (Camera_->Active->isThirdPerson)
	{
		obj->input.camLookX = (update.Pos.X - Camera_->CurrentPos.X);
		obj->input.camLookZ = (update.Pos.Z - Camera_->CurrentPos.Z);
	}
	else
	{
		obj->input.camLookX = (update.Pos.X - Camera_->CurrentPos.X) + (Math_Sin((-Entities_->List[ENTITIES_SELF_ID]->Yaw + 180) * MATH_DEG2RAD));
		obj->input.camLookZ = (update.Pos.Z - Camera_->CurrentPos.Z) + (Math_Cos((-Entities_->List[ENTITIES_SELF_ID]->Yaw + 180) * MATH_DEG2RAD));
	}

	if (obj->state.action & ACT_FLAG_ON_POLE)
	{
		obj->input.camLookX *= -1;
		obj->input.camLookZ *= -1;
		obj->input.stickX *= -1;
		obj->input.stickY *= -1;
	}

	obj->input.buttonA = KeyBind_IsPressed(KEYBIND_JUMP);
	obj->input.buttonB = KeyBind_IsPressed(KEYBIND_DELETE_BLOCK);
	// Z button is managed in Classic64_events.c
	/*
	Entities_->List[ENTITIES_SELF_ID]->Position.X = marioState.position[0];
	Entities_->List[ENTITIES_SELF_ID]->Position.Y = marioState.position[1];
	Entities_->List[ENTITIES_SELF_ID]->Position.Z = marioState.position[2];
	*/
}

// main plugin functions
static void Classic64_Init()
{
	LoadSymbolsFromGame();
	for (int i=0; i<256; i++) marioInstances[i] = 0;
	loadSettings();
	inited = false;

	FILE *f = fopen("plugins/sm64.us.z64", "rb");

	if (!f)
	{
		SendChat("&cSuper Mario 64 US ROM not found!", NULL, NULL, NULL, NULL);
		SendChat("&cClassic64 will not function without it.", NULL, NULL, NULL, NULL);
		SendChat("&cPlease supply a ROM with filename 'sm64.us.z64'", NULL, NULL, NULL, NULL);
		SendChat("&cand place it inside the plugins folder.", NULL, NULL, NULL, NULL);
		return;
	}

	// load ROM into memory
	uint8_t *romBuffer;
	size_t romFileLength;

	fseek(f, 0, SEEK_END);
	romFileLength = (size_t)ftell(f);
	rewind(f);
	romBuffer = (uint8_t*)malloc(romFileLength + 1);
	fread(romBuffer, 1, romFileLength, f);
	romBuffer[romFileLength] = 0;
	fclose(f);

	// perform SHA-1 check to make sure it's the correct ROM
	char expectedHash[] = "9bef1128717f958171a4afac3ed78ee2bb4e86ce";
	char hashResult[21];
	char hashHexResult[41];
	SHA1(hashResult, (char*)romBuffer, romFileLength);

	for( int offset = 0; offset < 20; offset++)
		sprintf( ( hashHexResult + (2*offset)), "%02x", hashResult[offset]&0xff);

	if (strcmp(hashHexResult, expectedHash)) // mismatch
	{
		cc_string expected = String_FromConst(expectedHash);
		cc_string result = String_FromConst(hashHexResult);
		SendChat("&cSuper Mario 64 US ROM is not correct! (SHA-1 mismatch)", NULL, NULL, NULL, NULL);
		SendChat("&cExpected: &e%s", &expected, NULL, NULL, NULL);
		SendChat("&cYour copy: &e%s", &result, NULL, NULL, NULL);
		SendChat("&cClassic64 will not function with this ROM.", NULL, NULL, NULL, NULL);
		return;
	}

	// all good!
	inited = true;
	allowTick = false;
	ticksBeforeSpawn = 1;
	marioInterpTicks = 0;

	// Mario texture is 704x64 RGBA (changed to 1024x64 in this classicube plugin)
	marioTextureUint8 = (uint8_t*)malloc(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
	marioBitmap.scan0 = (BitmapCol*)malloc(sizeof(BitmapCol) * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT); // texture sizes must be power of two

	// load libsm64
	sm64_global_terminate();
	sm64_global_init(romBuffer, marioTextureUint8, NULL);
	for (int i=0; i<SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT; i++) // copy texture to classicube bitmap
	{
		int x = i % 1024;
		if (x < 704)
		{
			cc_uint8 a = marioTextureUint8[i*4+3];
			cc_uint8 r = marioTextureUint8[i*4+0];
			cc_uint8 g = marioTextureUint8[i*4+1];
			cc_uint8 b = marioTextureUint8[i*4+2];
			marioBitmap.scan0[i] = b + (g << 8) + (r << 16) + (a << 24);
		}
		else
		{
			// fill X pos above 704 with white, for use with untextured (colored) triangles
			marioBitmap.scan0[i] = 255 + (255 << 8) + (255 << 16) + (255 << 24);
		}
	}
	free(marioTextureUint8);
	free(romBuffer);

	marioTextureID = Gfx_CreateTexture(&marioBitmap, 0, false);

	SendChat("&aSuper Mario 64 US ROM loaded!", NULL, NULL, NULL, NULL);
	if (!pluginOptions[PLUGINOPTION_FIRST_USE].value.on)
	{
		// give a little tutorial
		pluginOptions[PLUGINOPTION_FIRST_USE].value.on = true;
		saveSettings();
		SendChat("&bTo get started, switch to Mario by using &e/model mario64", NULL, NULL, NULL, NULL);
		SendChat("&bYou can find some extra commands by typing &e/client mario64", NULL, NULL, NULL, NULL);
		SendChat("&bHave fun playing as Mario!", NULL, NULL, NULL, NULL);
	}

	String_AppendConst(&Server_->AppName, " + Classic64 Mario64 v1.0");

	Model_Register(marioModel_GetInstance());
	Event_Register_(&ChatEvents_->ChatReceived, NULL, OnChatMessage);
	Event_Register_(&InputEvents_->Down, NULL, OnKeyDown);
	Event_Register_(&InputEvents_->Up, NULL, OnKeyUp);
	Event_Register_(&GfxEvents_->ContextLost, NULL, OnContextLost);
	Event_Register_(&GfxEvents_->ContextRecreated, NULL, OnContextRecreated);

	ScheduledTask_Add(1./30, marioTick);
	ScheduledTask_Add(1./300, selfMarioTick);
	Commands_Register(&MarioClientCmd);
}

static void Classic64_Free()
{
	if (!inited) return;
	allowTick = false;
	for (int i=0; i<256; i++)
		deleteMario(i);
}

static void Classic64_Reset()
{
	if (!inited) return;
	allowTick = false;
	for (int i=0; i<256; i++)
		deleteMario(i);
}

static void Classic64_OnNewMap()
{
	if (!inited) return;
	allowTick = false;
	for (int i=0; i<256; i++)
		deleteMario(i);
}

static void Classic64_OnNewMapLoaded()
{
	if (!inited) return;
	allowTick = true;
	ticksBeforeSpawn = 1;
}

EXPORT int Plugin_ApiVersion = 1;
EXPORT struct IGameComponent Plugin_Component = {
	Classic64_Init,
	Classic64_Free,
	Classic64_Reset,
	Classic64_OnNewMap,
	Classic64_OnNewMapLoaded
};

#define QUOTE(x) #x

#ifdef CC_BUILD_WIN
#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#include <windows.h>
#define LoadSymbol(name) name ## _ = GetProcAddress(app, QUOTE(name))
#else
#define _GNU_SOURCE
#include <dlfcn.h>
#define LoadSymbol(name) name ## _ = dlsym(RTLD_DEFAULT, QUOTE(name))
#endif

static void LoadSymbolsFromGame(void) {
#ifdef CC_BUILD_WIN
	HMODULE app = GetModuleHandle(NULL);
#endif
	LoadSymbol(Server); 
	LoadSymbol(Blocks);
	LoadSymbol(Models); 
	LoadSymbol(Game);
	LoadSymbol(World);
	LoadSymbol(Env);
	LoadSymbol(Entities);
	LoadSymbol(Camera);
	LoadSymbol(Gui);
	LoadSymbol(Lighting);
	LoadSymbol(ChatEvents);
	LoadSymbol(GfxEvents);
	LoadSymbol(InputEvents);
}
