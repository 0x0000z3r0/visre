#include <raylib.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#define _GNU_SOURCE
#include <sys/uio.h>
#include <syscall.h>

extern ssize_t 
process_vm_readv(pid_t pid,        
		const struct iovec *local_iov,
                unsigned long liovcnt,
                const struct iovec *remote_iov,
                unsigned long riovcnt,
                unsigned long flags);

#include "rcamera.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

typedef struct {
	void *start;
	void *end;
} reg;

typedef struct {
	unsigned int len;
	unsigned int cap;

	reg *regs;
} reg_vec;

enum {
	PLOT_HEX,
	PLOT_BYTE,
	PLOT_CUBE,
	PLOT_SPHERE,
	PLOT_MAP,
	PLOT_NODE,
	PLOT_PHC
};

int 
main(int argc, char *argv[])
{
	int win_width;
	int win_height;

	SetConfigFlags(FLAG_WINDOW_RESIZABLE);

	win_width = 1200;
	win_height = 700;
	InitWindow(win_width, win_height, "visre");
	SetTargetFPS(60);

	Camera3D camera;
	Vector3 camera_pos;
	camera_pos.x = 40.0f;
	camera_pos.y = 40.0f;
	camera_pos.z = 40.0f;
	camera.position = camera_pos;

	Vector3 camera_target;
	camera_target.x = 0.0f;
	camera_target.y = 0.0f;
	camera_target.z = 0.0f;
	camera.target = camera_target;

	Vector3 camera_up;
	camera_up.x = 0.0f;
	camera_up.y = 1.0f;
	camera_up.z = 0.0f;
	camera.up = camera_up;
	camera.fovy = 45.0f;
	camera.projection = CAMERA_PERSPECTIVE;

	GuiLoadStyle("terminal.rgs");

	int plot_state;
	plot_state = PLOT_CUBE;

	int file_size;
	file_size = 0;

	unsigned char *file_data;
	file_data = NULL;

	float sec_size;
	sec_size = 10000;

	float sec_off;
	sec_off = 0;

	char status_bar_txt[256] = "Unloaded";

	int file_mem;
	file_mem = 0;

	reg_vec file_addrs;
	file_addrs.len = 0;
	file_addrs.cap = 4;
	file_addrs.regs = NULL;

	char **file_addrs_lst;
	file_addrs_lst = NULL;

	pid_t pid;
	if (argc > 1) {
		if (strcmp(argv[1], "-m") == 0) {
			file_mem = 1;

			char path[PATH_MAX];

			sscanf(argv[2], "%i", &pid);

			const char proc_str[] = "/proc/";
			strcpy(path, proc_str);

			const size_t pid_len = strlen(argv[2]);
			strcpy(path + sizeof (proc_str) - 1, argv[2]);

			const char maps_str[] = "/maps";
			strcpy(path + sizeof (proc_str) - 1 + pid_len, maps_str);

			TraceLog(LOG_INFO, path);

			file_addrs.regs = malloc(file_addrs.cap * sizeof (reg));
			if (file_addrs.regs == NULL) {
				TraceLog(LOG_ERROR, "Failed to allocate PROC address vector");
				exit(1);
			}

			FILE *maps;
			maps = fopen(path, "rb");
			if (maps == NULL) {
				TraceLog(LOG_ERROR, "Failed to open the file");
				exit(1);
			}

			char line[1024];
			while (1) {
				fgets(line, sizeof (line), maps);
				if (feof(maps))
					break;

				long start, end;
				sscanf(line, "%lx-%lx", &start, &end);

				if (file_addrs.cap == file_addrs.len) {
					file_addrs.cap *= 2;

					reg *regs;
					regs = realloc(file_addrs.regs, file_addrs.cap * sizeof (reg));
					if (regs == NULL) {
						TraceLog(LOG_ERROR, "Failed to reallocate PROC address vector");
						exit(1);
					}

					file_addrs.regs = regs;
				}

				file_addrs.regs[file_addrs.len].start = (void*)start;
				file_addrs.regs[file_addrs.len].end = (void*)end;

				++file_addrs.len;
			}

			fclose(maps);

			file_addrs_lst = malloc(file_addrs.len * sizeof (char*));
			if (file_addrs_lst == NULL) {
				TraceLog(LOG_ERROR, "Failed to allocate PROC address list");
				exit(1);
			}

			for (unsigned int i = 0; i < file_addrs.len; ++i) {
				file_addrs_lst[i] = malloc(64);

				sprintf(file_addrs_lst[i], "%08lX - %08lX", 
					(long)file_addrs.regs[i].start, 
					(long)file_addrs.regs[i].end);
			}
		} else {
			const char *file_name;
			file_name = GetFileName(argv[1]);

			unsigned char *data;
			data = LoadFileData(argv[1], &file_size);

			sprintf(status_bar_txt, "name:%s, size:%i", file_name, file_size);

			if (file_data != NULL)
				free(file_data);

			file_data = malloc(file_size);
			if (file_data != NULL) {
				memcpy(file_data, data, file_size);
			}
		}
	}

	int addrs_scroll_idx;
	int addrs_curr_idx;
	int addrs_focus_idx;

	addrs_scroll_idx = 0;
	addrs_curr_idx = 0;
	addrs_focus_idx = -1;

	int map_model_cache;
	map_model_cache = 0;

	Model map_model;
	Image map_img;
	Mesh map_mesh;
	Texture2D map_tex;

	char pattern_txt[512];
	unsigned char pattern_buf[256]; 

	bzero(pattern_txt, sizeof (pattern_txt));
	bzero(pattern_buf, sizeof (pattern_buf));

	float hex_txt_off;
	int hex_txt_cache;
	char hex_txt_buf[2048];
	char str_txt_buf[1024];

	hex_txt_off = sec_off;
	hex_txt_cache = 0;

	int phc_ord;
	int phc_reg;
	int phc_cnt;
	int phc_rgb;
	int phc_len;
	float phc_thc;
	phc_ord = 9;
	phc_reg = (int)powf(2, phc_ord);
	phc_cnt = phc_reg * phc_reg;
	phc_rgb = 0;
	phc_len = 1024;
	phc_thc = 1.0f;

	while (!WindowShouldClose()) {
		if (IsFileDropped()) {
			FilePathList file_paths;
			file_paths = LoadDroppedFiles();

			if (file_paths.count == 1) {
				char *path;
				path = file_paths.paths[0];
				
				const char *file_name;
				file_name = GetFileName(path);

				unsigned char *data;
				data = LoadFileData(path, &file_size);

				sprintf(status_bar_txt, "name:%s, size:%i", file_name, file_size);

				if (file_data != NULL)
					free(file_data);

				file_data = malloc(file_size);
				if (file_data != NULL) {
					memcpy(file_data, data, file_size);
				}
			}
			UnloadDroppedFiles(file_paths);
		}

		if (file_mem) {
			file_size = file_addrs.regs[addrs_curr_idx].end - file_addrs.regs[addrs_curr_idx].start;
			file_data = malloc(file_size);

			struct iovec local[1];
			local[0].iov_base = file_data;
			local[0].iov_len = file_size;

			struct iovec remote[1];
			remote[0].iov_base = file_addrs.regs[addrs_curr_idx].start;
			remote[0].iov_len = file_size;

			int res;
			res = process_vm_readv(pid, local, 1, remote, 1, 0);
			
			char *res_txt = "OK";
			if (res == -1)
				res_txt = strerror(errno);
			else if (res < file_size)
				res_txt = "Missing bytes";

			sprintf(status_bar_txt, "pid:%i, size:%i, reg:%p-%p, res:%s", 
				pid, file_size, remote[0].iov_base, remote[0].iov_base + remote->iov_len, res_txt);
		}

		if (plot_state == PLOT_CUBE 
			|| plot_state == PLOT_SPHERE
			|| plot_state == PLOT_MAP) {
			if (IsKeyDown(KEY_LEFT))
				CameraYaw(&camera, 0.1f, 1);
			if (IsKeyDown(KEY_RIGHT))
				CameraYaw(&camera, -0.1f, 1);
			if (IsKeyDown(KEY_UP))
				CameraPitch(&camera, 0.1f, 1, 1, 1);
			if (IsKeyDown(KEY_DOWN))
				CameraPitch(&camera, -0.1f, 1, 1, 1);
			if (IsKeyDown(KEY_KP_ADD))
				UpdateCameraPro(&camera, (Vector3){ 0.0f, 0.0f, 0.0f }, (Vector3){ 0.0f, 0.0f, 0.0f }, -1.0f);
			if (IsKeyDown(KEY_KP_SUBTRACT))
				UpdateCameraPro(&camera, (Vector3){ 0.0f, 0.0f, 0.0f }, (Vector3){ 0.0f, 0.0f, 0.0f }, 1.0f);
		}

		BeginDrawing();
		{
			ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

			if (file_size == 0) {
				int old_txt_align;
				old_txt_align = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
        			GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);

				Rectangle welcome_txt_rec;
				welcome_txt_rec.width = 150;
				welcome_txt_rec.height = 10;
				welcome_txt_rec.x = (float)GetScreenWidth() / 2 - welcome_txt_rec.width / 2;
				welcome_txt_rec.y = (float)GetScreenHeight() / 2 - welcome_txt_rec.height / 2;
        			GuiLabel(welcome_txt_rec, "Drag & Drop your file");
        			GuiSetStyle(LABEL, TEXT_ALIGNMENT, old_txt_align);
			} else {
				switch (plot_state) {
				case PLOT_HEX:
				{
					if (!hex_txt_cache) {
						for (int i = 0; i < sizeof (hex_txt_buf) / 2 - 2; ++i) {
							sprintf(hex_txt_buf + i * 2, "%02X", file_data[(int)sec_off + i]);

							char final;
							final = '.';
							if (file_data[(int)sec_off + i] > 0x19 && file_data[(int)sec_off + i] < 0x7F)
								final = file_data[(int)sec_off + i];
							sprintf(str_txt_buf + i, "%c", final);
						}
					}

					hex_txt_cache = 1;

					GuiSetStyle(DEFAULT, TEXT_ALIGNMENT_VERTICAL, TEXT_ALIGN_TOP);
					GuiSetStyle(DEFAULT, TEXT_WRAP_MODE, TEXT_WRAP_CHAR);

					Rectangle hex_rec;
					hex_rec.width = (float)((GetScreenWidth() - 350 - 5) * 2.0f / 3.0f);
					hex_rec.x = 270;
					hex_rec.y = 100;
					hex_rec.height = (float)GetScreenHeight() - 30 - hex_rec.y;
					GuiTextBox(hex_rec, hex_txt_buf, sizeof (hex_txt_buf), 0);

					Rectangle str_rec;
					str_rec.width = (float)((GetScreenWidth() - 350 - 5) * 1.0f / 3.0f);
					str_rec.x = hex_rec.x + hex_rec.width + 5;
					str_rec.y = 100;
					str_rec.height = (float)GetScreenHeight() - 30 - str_rec.y;
					GuiTextBox(str_rec, str_txt_buf, sizeof (str_txt_buf), 0);

					GuiSetStyle(DEFAULT, TEXT_WRAP_MODE, TEXT_WRAP_NONE);
					GuiSetStyle(DEFAULT, TEXT_ALIGNMENT_VERTICAL, TEXT_ALIGN_MIDDLE);
					break;
				}
				case PLOT_BYTE:
				{
					unsigned int freqs[256];
					bzero(freqs, sizeof (freqs));
					
					float entropy;
					entropy = 0.0f;
						
					GuiLabel((Rectangle){ GetScreenWidth() - 210, 84, 100, 20 }, TextFormat("(0x19...0x7F)"));

					Color ascii_color = PURPLE;
					DrawRectangle(GetScreenWidth() - 132, 83, 84, 24, WHITE);
					DrawRectangle(GetScreenWidth() - 130, 85, 80, 20, ascii_color);

					GuiLabel((Rectangle){ GetScreenWidth() - 210, 114, 100, 20 }, TextFormat("[0x00]"));

					Color zero_color = RAYWHITE;
					DrawRectangle(GetScreenWidth() - 132, 113, 84, 24, WHITE);
					DrawRectangle(GetScreenWidth() - 130, 115, 80, 20, zero_color);

					GuiLabel((Rectangle){ GetScreenWidth() - 210, 144, 100, 20 }, TextFormat("[0xFF]"));

					Color fill_color = SKYBLUE;
					DrawRectangle(GetScreenWidth() - 132, 143, 84, 24, WHITE);
					DrawRectangle(GetScreenWidth() - 130, 145, 80, 20, fill_color);

					GuiLabel((Rectangle){ GetScreenWidth() - 210, 174, 100, 20 }, TextFormat("[0xXY]"));

					Color misc_color = BLUE;
					DrawRectangle(GetScreenWidth() - 132, 173, 84, 24, WHITE);
					DrawRectangle(GetScreenWidth() - 130, 175, 80, 20, misc_color);

					Color color;
					for (int i = 0; i < (int)sec_size; ++i) {
						if (file_data[(int)sec_off + i] > 0x19 && file_data[(int)sec_off + i] < 0x7F) {
							color = ascii_color;
						} else if (file_data[(int)sec_off + i] == 0x00) {
							color = zero_color;
						} else if (file_data[(int)sec_off + i] == 0xFF) {
							color = fill_color;
						} else {
							color = BLUE;
						}

						++freqs[file_data[(int)sec_off + i]];

						DrawRectangle((GetScreenWidth() / 2 - 200) + (i * 5) % 400, 150 + (i * 5) / 400, 5, 5, color);
					}

					for (int i = 0; i < 256; ++i) {
						float prob;
						prob = freqs[i] / sec_size;

						if (freqs[i] != 0) {
							entropy -= prob * log2f(prob);
						}
					}
					
					Rectangle ent_rec;
					ent_rec.width = 120;
					ent_rec.height = 20;
					ent_rec.x = (float)GetScreenWidth() - ent_rec.width - 20;
					ent_rec.y = 200;

					GuiLabel(ent_rec, TextFormat("Entropy: %.04f", entropy));
					break;
				}
				case PLOT_CUBE:
					BeginMode3D(camera);
					{
						Color color;
						for (int i = sec_off; i < (int)sec_off + (int)sec_size; ++i) {
							Vector3 vec;
							vec.x = (file_data[i + 0] - 128.0f) / 10.0f;
							vec.y = (file_data[i + 1] - 128.0f) / 10.0f;
							vec.z = (file_data[i + 2] - 128.0f) / 10.0f;

							if (file_data[(int)sec_off + i] > 0x19 && file_data[(int)sec_off + i] < 0x7F) {
								color = PURPLE;
							} else if (file_data[(int)sec_off + i] == 0x00) {
								color = RAYWHITE;
							} else if (file_data[(int)sec_off + i] == 0xFF) {
								color = SKYBLUE;
							} else {
								color = BLUE;
							}
							DrawPoint3D(vec, color);
						}
					}
					EndMode3D();
					break;
				case PLOT_SPHERE:
					BeginMode3D(camera);
					{
						for (int i = sec_off; i < (int)sec_off + (int)sec_size; ++i) {
							float x, y, z;
							x = (file_data[i + 0] - 128.0f) / 10.0f;
							y = (file_data[i + 1] - 128.0f) / 10.0f;
							z = (file_data[i + 2] - 128.0f) / 10.0f;

							float l;
							l = sqrtf(x * x + y * y + z * z);
							
							Vector3 vec;
							vec.x = x / l * 20.0f;
							vec.y = y / l * 20.0f;
							vec.z = z / l * 20.0f;

							DrawPoint3D(vec, BLUE);
						}
					}
					EndMode3D();
					break;
				case PLOT_MAP:
					BeginMode3D(camera);
					{
						if (!map_model_cache) {
							map_img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
							map_img.data = file_data + (int)sec_off;
							map_img.width = 100;
							map_img.height = 100;
							map_img.mipmaps = 1;

							map_tex = LoadTextureFromImage(map_img);
							map_mesh = GenMeshHeightmap(map_img, (Vector3){ 25.0f, 5.0f, 25.0f });
							map_model = LoadModelFromMesh(map_mesh);

							map_model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = map_tex;
						}

						map_model_cache = 1;

						DrawModelWires(map_model, (Vector3){ -20.0f, 0.0f, -20.0f }, 1.6f, BLUE);
						DrawGrid(20, 2.5f);
					}
					EndMode3D();

					DrawTexture(map_tex, GetScreenWidth() - map_tex.width - 20, 120, WHITE);
					DrawRectangleLines(GetScreenWidth() - map_tex.width - 20, 120, map_tex.width, map_tex.height, BLUE);
					break;
				case PLOT_NODE:
				{
					Rectangle pattern_rec;
					pattern_rec.width = 250;
					pattern_rec.height = 30;
					pattern_rec.x = (float)GetScreenWidth() / 2 - pattern_rec.width / 2;
					pattern_rec.y = 100;
					GuiTextBox(pattern_rec, pattern_txt, sizeof (pattern_txt), 1);

					Rectangle pattern_lab_rec;
					pattern_lab_rec.width = 250;
					pattern_lab_rec.height = 30;
					pattern_lab_rec.x = (float)GetScreenWidth() / 2 - pattern_lab_rec.width / 2;
					pattern_lab_rec.y = pattern_rec.y - pattern_rec.height - 5;
					GuiLabel(pattern_lab_rec, "Pattern");

					size_t len;
					len = strlen(pattern_txt);

					for (int i = 0; i < len / 2; ++i) {
						sscanf(pattern_txt + i * 2, "%02X", (int*)(pattern_buf + i));
					}

					for (int i = sec_off; i < (int)sec_off + (int)sec_size; ++i) {
						if (memcmp(file_data + i, pattern_buf, len) == 0) {
						}
					}
					break;
				}
				case PLOT_PHC:
				{
					int phc_pts[][2] = {
						{ 0, 0 },
						{ 0, 1 },
						{ 1, 1 },
						{ 1, 0 }
					};

					int *phc_lns;
					phc_lns = malloc(phc_cnt * sizeof (int) * 2);
					if (phc_lns == NULL)
						break;

					Color *color_file_data = (Color*)file_data;

					int sec_len = (int)sec_off + (int)sec_size;
					if (phc_rgb)
						sec_len /= sizeof (Color);

					Color color;
					int phc_rep = 2; 
					for (int i = sec_off; i < sec_len; ++i) {
						if (file_data[(int)sec_off + i] > 0x19 && file_data[(int)sec_off + i] < 0x7F) {
							color = PURPLE;
						} else if (file_data[(int)sec_off + i] == 0x00) {
							color = PINK;
						} else if (file_data[(int)sec_off + i] == 0xFF) {
							color = SKYBLUE;
						} else {
							color = BLUE;
						}

						for (int r = 0; r < phc_rep; ++r) {

						int k;
						k = (i - sec_off) * phc_rep + r;

						if (k >= phc_cnt)
							break;

						int o;
						o = k * 2;

						int m;
						m = k & 0x3;

						phc_lns[o + 0] = phc_pts[m][0];
						phc_lns[o + 1] = phc_pts[m][1];

						for (int j = 1; j < phc_ord; ++j) {
							int l;
							l = (int)powf(2, j);

							k = k >> 2;
							m = k & 0x3;	

							int t;
							switch (m) {
							case 0:
								t = phc_lns[o + 0];
								phc_lns[o + 0] = phc_lns[o + 1];
								phc_lns[o + 1] = t;
								break;
							case 1:
								phc_lns[o + 1] += l;
								break;
							case 2:
								phc_lns[o + 0] += l;
								phc_lns[o + 1] += l;
								break;
							case 3:
								t = l - 1 - phc_lns[o + 0];
								phc_lns[o + 0] = l - 1 - phc_lns[o + 1];
								phc_lns[o + 1] = t;

								phc_lns[o + 0] += l;
								break;
							}
						}

						phc_lns[o + 0] *= phc_len / phc_reg;
						phc_lns[o + 1] *= phc_len / phc_reg;

						if (o > 0) {
							Vector2 s;
							s.x = phc_lns[o + 0] / 2.0f + GetScreenWidth() / 2.0f - (phc_len / phc_ord * 2);
							s.y = phc_lns[o + 1] / 2.0f + 100;

							Vector2 e;
							e.x = phc_lns[o - 2 + 0] / 2.0f + GetScreenWidth() / 2.0f - (phc_len / phc_ord * 2);
							e.y = phc_lns[o - 2 + 1] / 2.0f + 100;

							if (phc_rgb)
								DrawLineEx(s, e, phc_thc, color_file_data[i]);
							else
								DrawLineEx(s, e, phc_thc, color);
						}

						}
					}

					free(phc_lns);

					GuiSliderBar((Rectangle){ GetScreenWidth() - 255, 90, 200, 20 }, NULL, 
						TextFormat("%.02f", phc_thc), &phc_thc, 1.0f, 20.0f);

					float phc_len_f = phc_len;
					GuiSliderBar((Rectangle){ GetScreenWidth() - 255, 120, 200, 20 }, NULL, 
						TextFormat("%i", phc_len), &phc_len_f, phc_reg, 8192 * 2);
					phc_len = (int)phc_len_f;

					float phc_ord_f = phc_ord;
					GuiSliderBar((Rectangle){ GetScreenWidth() - 255, 150, 200, 20 }, NULL, 
						TextFormat("%i", phc_ord), &phc_ord_f, 4, 12);
					phc_ord = (int)phc_ord_f;
					phc_reg = (int)powf(2, phc_ord);
					phc_cnt = phc_reg * phc_reg;

					if (GuiButton((Rectangle){ GetScreenWidth() - 155, 180, 100, 20 }, "Switch")) {
						phc_rgb ^= 1;
					}

					break;
				}
				}
			}

			if (GuiButton((Rectangle){ 10, 10, 100, 20 }, "Hex")) {
				plot_state = PLOT_HEX;
			}

			if (GuiButton((Rectangle){ 10, 40, 100, 20 }, "Byte")) {
				plot_state = PLOT_BYTE;
				sec_size = 25000;
			}
			
			if (GuiButton((Rectangle){ 10, 70, 100, 20 }, "Cube")) {
				plot_state = PLOT_CUBE;
			}

			if (GuiButton((Rectangle){ 10, 100, 100, 20 }, "Sphere")) {
				plot_state = PLOT_SPHERE;
			}

			if (GuiButton((Rectangle){ 10, 130, 100, 20 }, "Map")) {
				plot_state = PLOT_MAP;
				camera.position = camera_pos;
				camera.target = camera_target;
				camera.up = camera_up;
			}

			GuiSetState(STATE_DISABLED);
			if (GuiButton((Rectangle){ 10, 160, 100, 20 }, "Node")) {
				plot_state = PLOT_NODE;
			}
			GuiSetState(STATE_NORMAL);

			if (GuiButton((Rectangle){ 10, 190, 100, 20 }, "PHC")) {
				plot_state = PLOT_PHC;
				sec_size = phc_cnt;
			}


			if (file_size != 0) {
				Rectangle sec_off_rec;
				sec_off_rec.width = 200;
				sec_off_rec.height = 20;
				sec_off_rec.x = (float)GetScreenWidth() - 10 - sec_off_rec.width - 40;
				sec_off_rec.y = 10;
				if (GuiSliderBar(sec_off_rec, "Section Offset", TextFormat("%i", (int)sec_off), &sec_off, 0, file_size)) {
					hex_txt_cache = 0;	
					map_model_cache = 0;
				}

				if ((int)sec_off != (int)hex_txt_off) {
					hex_txt_cache = 0;
					map_model_cache = 0;
					hex_txt_off = sec_off;
				}

				Rectangle sec_size_rec;
				sec_size_rec.width = 200;
				sec_size_rec.height = 20;
				sec_size_rec.x = (float)GetScreenWidth() - 10 - sec_size_rec.width - 40;
				sec_size_rec.y = 40;

				int final_size;
				if (file_size - sec_off - 1 > 1)
					final_size = file_size - sec_off - 1;
				else
					final_size = 1;

				if (plot_state == PLOT_BYTE && final_size > 25000)
					final_size = 25000;
				if (plot_state == PLOT_PHC && final_size > phc_cnt)
					final_size = phc_cnt;

				GuiSliderBar(sec_size_rec, "Section Size", TextFormat("%i", (int)sec_size), &sec_size, 1, final_size);
			}

			if (file_mem) {
				Rectangle addrs_rec;
				addrs_rec.width = 250;
				addrs_rec.height = 250;
				addrs_rec.x = 10;
				addrs_rec.y = 220;

				GuiListViewEx(addrs_rec, (const char**)file_addrs_lst, file_addrs.len, &addrs_scroll_idx, &addrs_curr_idx, &addrs_focus_idx);
			}

			Rectangle status_bar_rec;
			status_bar_rec.width = (float)GetScreenWidth();
			status_bar_rec.height = 20;
			status_bar_rec.x = 0;
			status_bar_rec.y = (float)GetScreenHeight() - status_bar_rec.height;
			GuiStatusBar(status_bar_rec, status_bar_txt);
		}
		EndDrawing();
	}

	for (unsigned int i = 0; i < file_addrs.len; ++i)
		free(file_addrs_lst[i]);
	free(file_addrs_lst);
	free(file_addrs.regs);
				
	free(file_data);

	CloseWindow();

	return 0;
}
