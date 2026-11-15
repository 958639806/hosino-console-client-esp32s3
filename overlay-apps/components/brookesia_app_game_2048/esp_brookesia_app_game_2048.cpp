/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Phone-system adaptation of release/v0.6; original merge engine below.
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include "esp_lib_utils.h"
#include "esp_brookesia_app_game_2048.hpp"
#include "hosino_ui_fonts.h"

#define APP_NAME "2048"
#define ENABLE_CELL_DEBUG 0
#define GRID_BG_COLOR lv_color_hex(0x776456)
#define GRID_FONT &lv_font_montserrat_20
#define GRID_PAD 8
#define CELL_BG_COLOR lv_color_make(255, 255, 255)
#define CELL_RADIUS 5
#define CELL_SIZE ((short int)((_width - 5 * GRID_PAD) / 4))
#define CELL_OPA_1 LV_OPA_20
#define CELL_OPA_2 LV_OPA_COVER
#define ANIM_PERIOD 0
#define randint_between(min, max) (rand() % (max - min) + min)
#define rand_1_2() ((rand() % 10 == 0) ? 2 : 1)

LV_IMG_DECLARE(img_app_2048);
namespace esp_brookesia::apps {
namespace {
void plain(lv_obj_t *obj, uint32_t color, bool visible = true)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, visible ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}
lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y, const lv_font_t *font)
{
    auto label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf5efe5), 0);
    return label;
}
}
Game2048::Game2048() : App(APP_NAME, &img_app_2048, true, true, false) {}
Game2048::~Game2048() = default;
bool Game2048::init() { return true; }

bool Game2048::run()
{
    const int visual_width = lv_area_get_width(&getVisualArea());
    const int usable = lv_area_get_height(&getVisualArea()) - 68;
    if (visual_width < 280 || usable < 300) return false;
    const int side = std::min(visual_width - 88, usable - 130);
    _width = ((side - 5 * GRID_PAD) / 4) * 4 + 5 * GRID_PAD;
    _height = _width;
    _is_closing = false;
    _anim_running_flag = false;
    _generate_cell_flag = false;
    _finished = false;
    for (int i = 0; i < 16; ++i) {
        _cells_weight[i / 4][i % 4] = {1 << (i / 4), 1 << (i % 4), 0};
        _foreground_cells[i / 4][i % 4] = nullptr;
        _remove_ready_cells[i / 4][i % 4] = nullptr;
    }
    const uint32_t colors[11] = {0xeee4da, 0xede0c8, 0xf2b179, 0xf59563, 0xf67c5f,
                                0xf65e3b, 0xedcf72, 0xedcc61, 0xedc850, 0xedc53f, 0xedc22e};
    for (int i = 0; i < 11; ++i) _cell_colors[i] = lv_color_hex(colors[i]);
    srand(static_cast<unsigned>(time(nullptr)) ^ lv_tick_get());
    auto screen = lv_screen_active();
    plain(screen, 0x302a28);
    text(screen, "2048", 44, 10, &hosino_font_20);
    _status_label = text(screen, "方向键移动", visual_width - 188, 13, &hosino_font_14);
    _cur_score_label = text(screen, "得分 0", 44, 40, &hosino_font_16);
    _best_score_label = text(screen, "最高 0", 44, 61, &hosino_font_14);
    updateBestScore(_best_score);
    auto restart = lv_button_create(screen);
    lv_obj_set_pos(restart, visual_width - 156, 38);
    lv_obj_set_size(restart, 112, 36);
    lv_obj_set_style_bg_color(restart, lv_color_hex(0x8f7a66), 0);
    lv_obj_set_style_shadow_width(restart, 0, 0);
    auto restart_text = text(restart, "重新开始", 0, 0, &hosino_font_16);
    lv_obj_center(restart_text);
    lv_obj_add_event_cb(restart, new_game_event_cb, LV_EVENT_CLICKED, this);

    const int grid_x = (visual_width - _width) / 2;
    const int grid_y = 84;
    _game_grid = lv_obj_create(screen);
    plain(_game_grid, 0x776456);
    lv_obj_set_size(_game_grid, _width, _width);
    lv_obj_set_pos(_game_grid, grid_x, grid_y);
    for (int i = 0; i < 16; ++i) {
        auto cell = addBackgroundCell(_game_grid);
        _background_cells[i / 4][i % 4] = cell;
        lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
        lv_obj_set_pos(cell, GRID_PAD + (i % 4) * (CELL_SIZE + GRID_PAD),
                      GRID_PAD + (i / 4) * (CELL_SIZE + GRID_PAD));
    }
    _foreground_grid = lv_obj_create(screen);
    plain(_foreground_grid, 0, false);
    lv_obj_set_size(_foreground_grid, _width, _width);
    lv_obj_set_pos(_foreground_grid, grid_x, grid_y);
    lv_obj_set_style_text_font(_foreground_grid, GRID_FONT, 0);
    lv_obj_set_style_text_color(_foreground_grid, lv_color_hex(0x372b23), 0);
    lv_obj_remove_flag(_foreground_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(_game_grid);

    const char *arrows[] = {LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_RIGHT};
    const int button_width = 62, gap = 10;
    const int start_x = (visual_width - (button_width * 4 + gap * 3)) / 2;
    for (int i = 0; i < 4; ++i) {
        auto button = lv_button_create(screen);
        lv_obj_set_pos(button, start_x + i * (button_width + gap), usable - 38);
        lv_obj_set_size(button, button_width, 36);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x8f7a66), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<intptr_t>(i + 1)));
        auto arrow = text(button, arrows[i], 0, 0, &lv_font_montserrat_20);
        lv_obj_center(arrow);
        lv_obj_add_event_cb(button, motion_event_cb, LV_EVENT_CLICKED, this);
    }
    newGame();
    return true;
}

bool Game2048::back() { return notifyCoreClosed(); }
bool Game2048::pause() { _is_closing = true; return true; }
bool Game2048::resume() { _is_closing = false; return true; }
bool Game2048::close() { return pause(); }
bool Game2048::cleanResource()
{
    _is_closing = true;
    _cur_score_label = _best_score_label = _status_label = nullptr;
    _foreground_grid = _game_grid = nullptr;
    for (int i = 0; i < 16; ++i) {
        _background_cells[i / 4][i % 4] = nullptr;
        _foreground_cells[i / 4][i % 4] = nullptr;
        _remove_ready_cells[i / 4][i % 4] = nullptr;
    }
    // No global callbacks, timers or animations; Brookesia owns screen deletion.
    return true;
}

void Game2048::debugCells()
{
#if ENABLE_CELL_DEBUG
    debugCells(_foreground_cells);
    debugCells(_background_cells);
    debugCells(_cells_weight);
#endif
}

void Game2048::debugCells(int cell[4][4])
{
#if ENABLE_CELL_DEBUG
    ESP_UTILS_LOGD("cell");
    for (int i = 0; i < 4; i++) {
        ESP_UTILS_LOGD("\t%d\t%d\t%d\t%d", cell[i][0], cell[i][1], cell[i][2], cell[i][3]);
    }
#endif
}

void Game2048::debugCells(lv_obj_t *cell[4][4])
{
#if ENABLE_CELL_DEBUG
    for (int i = 0; i < 4; i++) {
        ESP_UTILS_LOGD("\t%p\t%p\t%p\t%p", cell[i][0], cell[i][1], cell[i][2], cell[i][3]);
    }
#endif
}

void Game2048::debugCells(cell_weight_t cell[4][4])
{
#if ENABLE_CELL_DEBUG
    for (int i = 0; i < 4; i++) {
        ESP_UTILS_LOGD(
            "\t(%d,%d,%d)\t(%d,%d,%d)\t(%d,%d,%d)\t(%d,%d,%d)",
            cell[i][0].x, cell[i][0].y, cell[i][0].weight,
            cell[i][1].x, cell[i][1].y, cell[i][1].weight,
            cell[i][2].x, cell[i][2].y, cell[i][2].weight,
            cell[i][3].x, cell[i][3].y, cell[i][3].weight
        );
    }
#endif
}

void Game2048::debugCells(lv_obj_t *cell[4])
{
#if ENABLE_CELL_DEBUG
    for (int i = 0; i < 4; i++) {
        ESP_UTILS_LOGD("\t%p\t%p\t%p\t%p", cell[i], cell[i], cell[i], cell[i]);
    }
#endif
}

void Game2048::cleanForegroundCells()
{
    lv_obj_t *child = lv_obj_get_child(_foreground_grid, 0);

    while (child) {
        lv_obj_del(child);
        child = lv_obj_get_child(_foreground_grid, 0);
    }
    for (int i = 0; i < 16; i++) {
        _cells_weight[i / 4][i % 4].weight = 0;
        _cells_weight[i / 4][i % 4].x = 1 << (i / 4);
        _cells_weight[i / 4][i % 4].y = 1 << (i % 4);
        _foreground_cells[i / 4][i % 4] = NULL;
    }
}

void Game2048::newGame()
{
    _finished = false;
    lv_label_set_text(_status_label, "方向键移动");
    _weight_max = 0;
    _current_score = 0;
    updateCurrentScore(_current_score);
    cleanForegroundCells();
    generateForegroundCell();
    generateForegroundCell();
    updateCellsStyle();
}

lv_obj_t *Game2048::addBackgroundCell(lv_obj_t *parent)
{
    lv_obj_t *cell = lv_obj_create(parent);
    // Shape
    lv_obj_set_style_radius(cell, CELL_RADIUS, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    // Background
    lv_obj_set_style_bg_color(cell, CELL_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(cell, CELL_OPA_1, 0);
    // Others
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    return cell;
}

void Game2048::generateForegroundCell()
{
    int zero_amount = 0, zero_index = 0;
    int target = 0;
    int target_i = 0;
    int target_j = 0;
    int target_weight = rand_1_2();

    _weight_max = (target_weight > _weight_max) ?
                  target_weight : _weight_max;

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (_cells_weight[i][j].weight == 0) {
                zero_amount++;
            }
        }
    }
    if (zero_amount == 0) {
        return;
    }

    target = randint_between(0, zero_amount);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if ((_cells_weight[i][j].weight == 0) &&
                    (zero_index++ == target)) {
                target_i = i;
                target_j = j;
                _cells_weight[i][j].weight = target_weight;
            }
        }
    }

    /* Add a new object of cell */
    lv_obj_t *cell = lv_obj_create(_foreground_grid);
    _foreground_cells[target_i][target_j] = cell;
    // Size
    lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
    // Position
    lv_obj_set_pos(
        cell,
        lv_obj_get_x(_background_cells[target_i][target_j]),
        lv_obj_get_y(_background_cells[target_i][target_j])
    );
    // Shape
    lv_obj_set_style_radius(cell, CELL_RADIUS, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    // Background
    lv_obj_set_style_bg_color(cell, CELL_BG_COLOR, 0);
    lv_obj_set_style_opa(cell, CELL_OPA_2, 0);
    // Others
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text_fmt(label, "%d", 1 << target_weight);
    lv_obj_center(label);

    debugCells();
}

void Game2048::addRemoveReadyCell(lv_obj_t *cell)
{
    if (cell == NULL) {
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (_remove_ready_cells[i / 4][i % 4] == cell) {
            return;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (_remove_ready_cells[i / 4][i % 4] == NULL) {
            _remove_ready_cells[i / 4][i % 4] = cell;
            return;
        }
    }
}

void Game2048::cleanRemoveReadyCell()
{
    for (int i = 0; i < 16; i++) {
        if (_remove_ready_cells[i / 4][i % 4] != NULL) {
            lv_obj_del(_remove_ready_cells[i / 4][i % 4]);
            _remove_ready_cells[i / 4][i % 4] = NULL;
        }
    }
}

// Finish a move within its LVGL event: closing cannot leave pending callbacks.
void Game2048::startAnimationX(lv_obj_t *target, int x, int) { lv_obj_set_x(target, x); }
void Game2048::startAnimationY(lv_obj_t *target, int y, int) { lv_obj_set_y(target, y); }

void Game2048::updateCellValue()
{
    for (int i = 0; i < 16; i++) {
        if (_foreground_cells[i / 4][i % 4] != NULL) {
            lv_obj_t *label = lv_obj_get_child(_foreground_cells[i / 4][i % 4], 0);
            int value = 1 << _cells_weight[i / 4][i % 4].weight;
            lv_label_set_text_fmt(label, "%d", value);
        }
    }
}

void Game2048::updateCurrentScore(uint32_t score)
{
    lv_label_set_text_fmt(_cur_score_label, "得分 %u", static_cast<unsigned>(score));
}

void Game2048::updateBestScore(uint32_t score)
{
    lv_label_set_text_fmt(_best_score_label, "最高 %u", static_cast<unsigned>(score));
}

void Game2048::updateCellsStyle()
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (_foreground_cells[i][j] != NULL && _cells_weight[i][j].weight > 0) {
                int color_index = _cells_weight[i][j].weight - 1;
                if (color_index >= 0 && color_index < 11) {
                    lv_obj_set_style_bg_color(
                        _foreground_cells[i][j],
                        _cell_colors[color_index],
                        0
                    );
                }
            }
        }
    }
}

int Game2048::maxWeight()
{
    return _weight_max;
}

int Game2048::moveLeft()
{
    int target_cells[4][4];
    int score = -1;

    for (int i = 0; i < 16; i++) {
        target_cells[i / 4][i % 4] = i % 4;
    }

    debugCells(_cells_weight);
    debugCells(target_cells);

    for (int i = 0; i < 4; i++) {
        for (int j = 1; j < 4; j++) {
            if (_cells_weight[i][j].weight != 0) {
                int merge_flag = false;
                int cur_j = j;
                while (cur_j != 0) {
                    if ((_cells_weight[i][cur_j - 1].weight == _cells_weight[i][cur_j].weight) &&
                            (!merge_flag) &&
                            ((_cells_weight[i][cur_j - 1].y & (_cells_weight[i][cur_j - 1].y - 1)) == 0)) {
                        merge_flag = true;
                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[i][cur_j].y & (1 << k);
                            if (target_flag) {
                                target_cells[i][k] = cur_j - 1;
                            }
                        }

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[i][cur_j - 1].y & (1 << k);
                            if (target_flag) {
                                addRemoveReadyCell(_foreground_cells[i][k]);
                            }
                        }

                        _cells_weight[i][cur_j - 1].y += _cells_weight[i][cur_j].y;
                        _cells_weight[i][cur_j - 1].weight++;
                        _cells_weight[i][cur_j].weight = 0;
                        _weight_max = (_cells_weight[i][cur_j - 1].weight > _weight_max) ?
                                      _cells_weight[i][cur_j - 1].weight : _weight_max;

                        score = (score < 0) ? 0 : score;
                        score += 1 << _cells_weight[i][cur_j - 1].weight;
                    } else if (_cells_weight[i][cur_j - 1].weight == 0) {

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[i][cur_j].y & (1 << k);
                            if (target_flag) {
                                target_cells[i][k] = cur_j - 1;
                            }
                        }

                        score = (score < 0) ? 0 : score;
                        _cells_weight[i][cur_j - 1] = _cells_weight[i][cur_j];
                        _cells_weight[i][cur_j].weight = 0;
                    } else {
                        break;
                    }
                    cur_j--;
                }
            }
        }
    }

    debugCells(_cells_weight);
    debugCells(target_cells);
    debugCells(_remove_ready_cells);
    debugCells(_foreground_cells);

    for (int i = 0; i < 4; i++) {
        for (int j = 1; j < 4; j++) {
            int target_j = target_cells[i][j];
            if ((_foreground_cells[i][j] != NULL) && (j != target_j)) {
                // printf(NULL, "i,j: %d,%d", i, j);
                int target_x = lv_obj_get_x(_background_cells[i][target_j]);
                startAnimationX(_foreground_cells[i][j], target_x, ANIM_PERIOD);
                _foreground_cells[i][target_j] = _foreground_cells[i][j];
                _foreground_cells[i][j] = NULL;
            }
        }
    }

    debugCells(_foreground_cells);

    // Reset x,y of weight
    for (int i = 0; i < 16; i++) {
        _cells_weight[i / 4][i % 4].x = 1 << (i / 4);
        _cells_weight[i / 4][i % 4].y = 1 << (i % 4);
    }

    return score;
}

int Game2048::moveRight()
{
    int target_cells[4][4];
    int score = -1;

    for (int i = 0; i < 16; i++) {
        target_cells[i / 4][i % 4] = i % 4;
    }

    debugCells(_cells_weight);
    debugCells(target_cells);

    for (int i = 0; i < 4; i++) {
        for (int j = 2; j >= 0; j--) {
            if (_cells_weight[i][j].weight != 0) {
                int merge_flag = false;
                int cur_j = j;
                while (cur_j != 3) {
                    if ((_cells_weight[i][cur_j + 1].weight == _cells_weight[i][cur_j].weight) &&
                            (!merge_flag) &&
                            ((_cells_weight[i][cur_j + 1].y & (_cells_weight[i][cur_j + 1].y - 1)) == 0)) {
                        merge_flag = true;
                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[i][cur_j].y & (1 << k);
                            if (target_flag) {
                                target_cells[i][k] = cur_j + 1;
                            }
                        }

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[i][cur_j + 1].y & (1 << k);
                            if (target_flag) {
                                addRemoveReadyCell(_foreground_cells[i][k]);
                            }
                        }

                        _cells_weight[i][cur_j + 1].y += _cells_weight[i][cur_j].y;
                        _cells_weight[i][cur_j + 1].weight++;
                        _cells_weight[i][cur_j].weight = 0;
                        _weight_max = (_cells_weight[i][cur_j + 1].weight > _weight_max) ?
                                      _cells_weight[i][cur_j + 1].weight : _weight_max;

                        score = (score < 0) ? 0 : score;
                        score += 1 << _cells_weight[i][cur_j + 1].weight;
                    } else if (_cells_weight[i][cur_j + 1].weight == 0) {

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[i][cur_j].y & (1 << k);
                            if (target_flag) {
                                target_cells[i][k] = cur_j + 1;
                            }
                        }

                        score = (score < 0) ? 0 : score;
                        _cells_weight[i][cur_j + 1] = _cells_weight[i][cur_j];
                        _cells_weight[i][cur_j].weight = 0;
                    } else {
                        break;
                    }
                    cur_j++;
                }
            }
        }
    }

    debugCells(_cells_weight);
    debugCells(target_cells);
    debugCells(_remove_ready_cells);
    debugCells(_foreground_cells);

    for (int i = 0; i < 4; i++) {
        for (int j = 2; j >= 0; j--) {
            int target_j = target_cells[i][j];
            if ((_foreground_cells[i][j] != NULL) && (j != target_j)) {
                // printf(NULL, "i,j: %d,%d", i, j);
                int target_x = lv_obj_get_x(_background_cells[i][target_j]);
                startAnimationX(_foreground_cells[i][j], target_x, ANIM_PERIOD);
                _foreground_cells[i][target_j] = _foreground_cells[i][j];
                _foreground_cells[i][j] = NULL;
            }
        }
    }

    debugCells(_foreground_cells);

    // Reset x,y of weight
    for (int i = 0; i < 16; i++) {
        _cells_weight[i / 4][i % 4].x = 1 << (i / 4);
        _cells_weight[i / 4][i % 4].y = 1 << (i % 4);
    }

    return score;
}

int Game2048::moveUp()
{
    int target_cells[4][4];
    int score = -1;

    for (int i = 0; i < 16; i++) {
        target_cells[i / 4][i % 4] = i / 4;
    }

    debugCells(_cells_weight);
    debugCells(target_cells);

    for (int j = 0; j < 4; j++) {
        for (int i = 1; i < 4; i++) {
            if (_cells_weight[i][j].weight != 0) {
                int merge_flag = false;
                int cur_i = i;
                while (cur_i != 0) {
                    if ((_cells_weight[cur_i - 1][j].weight == _cells_weight[cur_i][j].weight) &&
                            (!merge_flag) &&
                            ((_cells_weight[cur_i - 1][j].x & (_cells_weight[cur_i - 1][j].x - 1)) == 0)) {
                        merge_flag = true;
                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[cur_i][j].x & (1 << k);
                            if (target_flag) {
                                target_cells[k][j] = cur_i - 1;
                            }
                        }

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[cur_i - 1][j].x & (1 << k);
                            if (target_flag) {
                                addRemoveReadyCell(_foreground_cells[k][j]);
                            }
                        }

                        _cells_weight[cur_i - 1][j].x += _cells_weight[cur_i][j].x;
                        _cells_weight[cur_i - 1][j].weight++;
                        _cells_weight[cur_i][j].weight = 0;
                        _weight_max = (_cells_weight[cur_i - 1][j].weight > _weight_max) ?
                                      _cells_weight[cur_i - 1][j].weight : _weight_max;

                        score = (score < 0) ? 0 : score;
                        score += 1 << _cells_weight[cur_i - 1][j].weight;
                    } else if (_cells_weight[cur_i - 1][j].weight == 0) {

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[cur_i][j].x & (1 << k);
                            if (target_flag) {
                                target_cells[k][j] = cur_i - 1;
                            }
                        }

                        score = (score < 0) ? 0 : score;
                        _cells_weight[cur_i - 1][j] = _cells_weight[cur_i][j];
                        _cells_weight[cur_i][j].weight = 0;
                    } else {
                        break;
                    }
                    cur_i--;
                }
            }
        }
    }

    debugCells(_cells_weight);
    debugCells(target_cells);
    debugCells(_remove_ready_cells);
    debugCells(_foreground_cells);

    for (int j = 0; j < 4; j++) {
        for (int i = 1; i < 4; i++) {
            int target_i = target_cells[i][j];
            if ((_foreground_cells[i][j] != NULL) && (i != target_i)) {
                // printf(NULL, "i,j: %d,%d", i, j);
                int target_y = lv_obj_get_y(_background_cells[target_i][j]);
                startAnimationY(_foreground_cells[i][j], target_y, ANIM_PERIOD);
                _foreground_cells[target_i][j] = _foreground_cells[i][j];
                _foreground_cells[i][j] = NULL;
            }
        }
    }

    debugCells(_foreground_cells);

    // Reset x,y of weight
    for (int i = 0; i < 16; i++) {
        _cells_weight[i / 4][i % 4].x = 1 << (i / 4);
        _cells_weight[i / 4][i % 4].y = 1 << (i % 4);
    }

    return score;
}

int Game2048::moveDown()
{
    int target_cells[4][4];
    int score = -1;

    for (int i = 0; i < 16; i++) {
        target_cells[i / 4][i % 4] = i / 4;
    }

    debugCells(_cells_weight);
    debugCells(target_cells);

    for (int j = 0; j < 4; j++) {
        for (int i = 2; i >= 0; i--) {
            if (_cells_weight[i][j].weight != 0) {
                int merge_flag = false;
                int cur_i = i;
                while (cur_i != 3) {
                    if ((_cells_weight[cur_i + 1][j].weight == _cells_weight[cur_i][j].weight) &&
                            (!merge_flag) &&
                            ((_cells_weight[cur_i + 1][j].x & (_cells_weight[cur_i + 1][j].x - 1)) == 0)) {
                        merge_flag = true;
                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[cur_i][j].x & (1 << k);
                            if (target_flag) {
                                target_cells[k][j] = cur_i + 1;
                            }
                        }

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[cur_i + 1][j].x & (1 << k);
                            if (target_flag) {
                                addRemoveReadyCell(_foreground_cells[k][j]);
                            }
                        }

                        _cells_weight[cur_i + 1][j].x += _cells_weight[cur_i][j].x;
                        _cells_weight[cur_i + 1][j].weight++;
                        _cells_weight[cur_i][j].weight = 0;
                        _weight_max = (_cells_weight[cur_i + 1][j].weight > _weight_max) ?
                                      _cells_weight[cur_i + 1][j].weight : _weight_max;

                        score = (score < 0) ? 0 : score;
                        score += 1 << _cells_weight[cur_i + 1][j].weight;
                    } else if (_cells_weight[cur_i + 1][j].weight == 0) {

                        for (int k = 0; k < 4; k++) {
                            int target_flag = _cells_weight[cur_i][j].x & (1 << k);
                            if (target_flag) {
                                target_cells[k][j] = cur_i + 1;
                            }
                        }

                        score = (score < 0) ? 0 : score;
                        _cells_weight[cur_i + 1][j] = _cells_weight[cur_i][j];
                        _cells_weight[cur_i][j].weight = 0;
                    } else {
                        break;
                    }
                    cur_i++;
                }
            }
        }
    }

    debugCells(_cells_weight);
    debugCells(target_cells);
    debugCells(_remove_ready_cells);
    debugCells(_foreground_cells);

    for (int j = 0; j < 4; j++) {
        for (int i = 2; i >= 0; i--) {
            int target_i = target_cells[i][j];
            if ((_foreground_cells[i][j] != NULL) && (i != target_i)) {
                // printf(NULL, "i,j: %d,%d", i, j);
                int target_y = lv_obj_get_y(_background_cells[target_i][j]);
                startAnimationY(_foreground_cells[i][j], target_y, ANIM_PERIOD);
                _foreground_cells[target_i][j] = _foreground_cells[i][j];
                _foreground_cells[i][j] = NULL;
            }
        }
    }

    debugCells(_foreground_cells);

    // Reset x,y of weight
    for (int i = 0; i < 16; i++) {
        _cells_weight[i / 4][i % 4].x = 1 << (i / 4);
        _cells_weight[i / 4][i % 4].y = 1 << (i % 4);
    }

    return score;
}

bool Game2048::isGameOver()
{
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const int weight = _cells_weight[row][col].weight;
            if (!weight) return false;
            if (row < 3 && weight == _cells_weight[row + 1][col].weight) return false;
            if (col < 3 && weight == _cells_weight[row][col + 1].weight) return false;
        }
    }
    return true;
}

void Game2048::new_game_event_cb(lv_event_t *event)
{
    auto app = static_cast<Game2048 *>(lv_event_get_user_data(event));
    if (app && !app->_is_closing) app->newGame();
}

void Game2048::motion_event_cb(lv_event_t *event)
{
    auto app = static_cast<Game2048 *>(lv_event_get_user_data(event));
    if (!app || app->_is_closing || app->_finished) return;
    const auto direction = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    int score = -1;
    switch (direction) {
    case 1: score = app->moveLeft(); break;
    case 2: score = app->moveUp(); break;
    case 3: score = app->moveDown(); break;
    case 4: score = app->moveRight(); break;
    default: return;
    }
    app->finishMove(score);
}

void Game2048::finishMove(int score)
{
    cleanRemoveReadyCell();
    updateCellValue();
    if (score >= 0) {
        _current_score += static_cast<uint32_t>(score);
        updateCurrentScore(_current_score);
        if (_current_score > _best_score) {
            _best_score = _current_score;
            updateBestScore(_best_score);
        }
        generateForegroundCell();
    }
    updateCellsStyle();
    if (maxWeight() >= 11) {
        _finished = true;
        lv_label_set_text(_status_label, "已达成 2048");
    } else if (isGameOver()) {
        _finished = true;
        lv_label_set_text(_status_label, "游戏结束");
    }
}

ESP_UTILS_REGISTER_PLUGIN(systems::base::App, Game2048, APP_NAME)
} // namespace esp_brookesia::apps
