/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@clifford.at>
 *  Copyright (C) 2018  David Shah <david@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#include "bitstream.h"
#include <vector>
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

inline TileType tile_at(const Chip &chip, int x, int y)
{
    return chip.chip_info.tile_grid[y * chip.chip_info.width + x];
}

const ConfigEntryPOD &find_config(const TileInfoPOD &tile,
                                  const std::string &name)
{
    for (int i = 0; i < tile.num_config_entries; i++) {
        if (std::string(tile.entries[i].name) == name) {
            return tile.entries[i];
        }
    }
    assert(false);
}

std::tuple<int8_t, int8_t, int8_t> get_ieren(const BitstreamInfoPOD &bi,
                                             int8_t x, int8_t y, int8_t z)
{
    for (int i = 0; i < bi.num_ierens; i++) {
        auto ie = bi.ierens[i];
        if (ie.iox == x && ie.ioy == y && ie.ioz == z) {
            return std::make_tuple(ie.ierx, ie.iery, ie.ierz);
        }
    }
    // No pin at this location
    return std::make_tuple(-1, -1, -1);
};

void set_config(const TileInfoPOD &ti,
                std::vector<std::vector<int8_t>> &tile_cfg,
                const std::string &name, bool value, int index = -1)
{
    const ConfigEntryPOD &cfg = find_config(ti, name);
    if (index == -1) {
        for (int i = 0; i < cfg.num_bits; i++) {
            int8_t &cbit = tile_cfg.at(cfg.bits[i].row).at(cfg.bits[i].col);
            if (cbit && !value)
                log_error("clearing already set config bit %s", name.c_str());
            cbit = value;
        }
    } else {
        int8_t &cbit = tile_cfg.at(cfg.bits[index].row).at(cfg.bits[index].col);
        cbit = value;
        if (cbit && !value)
            log_error("clearing already set config bit %s[%d]", name.c_str(),
                      index);
    }
}

int get_param_or_def(const CellInfo *cell, const std::string &param,
                     int defval = 0)
{
    auto found = cell->params.find(param);
    if (found != cell->params.end())
        return std::stoi(found->second);
    else
        return defval;
}

std::string get_param_str_or_def(const CellInfo *cell, const std::string &param,
                                 std::string defval = "")
{
    auto found = cell->params.find(param);
    if (found != cell->params.end())
        return found->second;
    else
        return defval;
}

char get_hexdigit(int i) { return std::string("0123456789ABCDEF").at(i); }

void write_asc(const Design &design, std::ostream &out)
{
    const Chip &chip = design.chip;
    // [y][x][row][col]
    const ChipInfoPOD &ci = chip.chip_info;
    const BitstreamInfoPOD &bi = *ci.bits_info;
    std::vector<std::vector<std::vector<std::vector<int8_t>>>> config;
    config.resize(ci.height);
    for (int y = 0; y < ci.height; y++) {
        config.at(y).resize(ci.width);
        for (int x = 0; x < ci.width; x++) {
            TileType tile = tile_at(chip, x, y);
            int rows = bi.tiles_nonrouting[tile].rows;
            int cols = bi.tiles_nonrouting[tile].cols;
            config.at(y).at(x).resize(rows, std::vector<int8_t>(cols));
        }
    }
    out << ".comment from next-pnr" << std::endl;

    switch (chip.args.type) {
    case ChipArgs::LP384:
        out << ".device 384" << std::endl;
        break;
    case ChipArgs::HX1K:
    case ChipArgs::LP1K:
        out << ".device 1k" << std::endl;
        break;
    case ChipArgs::HX8K:
    case ChipArgs::LP8K:
        out << ".device 8k" << std::endl;
        break;
    case ChipArgs::UP5K:
        out << ".device 5k" << std::endl;
        break;
    default:
        assert(false);
    }
    // Set pips
    for (auto pip : chip.getPips()) {
        if (chip.pip_to_net[pip.index] != IdString()) {
            const PipInfoPOD &pi = ci.pip_data[pip.index];
            const SwitchInfoPOD &swi = bi.switches[pi.switch_index];
            for (int i = 0; i < swi.num_bits; i++) {
                bool val =
                        (pi.switch_mask & (1 << ((swi.num_bits - 1) - i))) != 0;
                int8_t &cbit = config.at(swi.y)
                                       .at(swi.x)
                                       .at(swi.cbits[i].row)
                                       .at(swi.cbits[i].col);
                if (bool(cbit) != 0)
                    assert(false);
                cbit = val;
            }
        }
    }
    // Set logic cell config
    for (auto cell : design.cells) {
        BelId bel = cell.second->bel;
        if (bel == BelId()) {
            std::cout << "Found unplaced cell " << cell.first
                      << " while generating bitstream!" << std::endl;
            continue;
        }
        const BelInfoPOD &beli = ci.bel_data[bel.index];
        int x = beli.x, y = beli.y, z = beli.z;
        if (cell.second->type == "ICESTORM_LC") {
            TileInfoPOD &ti = bi.tiles_nonrouting[TILE_LOGIC];
            unsigned lut_init = get_param_or_def(cell.second, "LUT_INIT");
            bool neg_clk = get_param_or_def(cell.second, "NEG_CLK");
            bool dff_enable = get_param_or_def(cell.second, "DFF_ENABLE");
            bool async_sr = get_param_or_def(cell.second, "ASYNC_SR");
            bool set_noreset = get_param_or_def(cell.second, "SET_NORESET");
            bool carry_enable = get_param_or_def(cell.second, "CARRY_ENABLE");
            std::vector<bool> lc(20, false);
            // From arachne-pnr
            static std::vector<int> lut_perm = {
                    4, 14, 15, 5, 6, 16, 17, 7, 3, 13, 12, 2, 1, 11, 10, 0,
            };
            for (int i = 0; i < 16; i++) {
                if ((lut_init >> i) & 0x1)
                    lc.at(lut_perm.at(i)) = true;
            }
            lc.at(8) = carry_enable;
            lc.at(9) = dff_enable;
            lc.at(18) = set_noreset;
            lc.at(19) = async_sr;

            for (int i = 0; i < 20; i++)
                set_config(ti, config.at(y).at(x), "LC_" + std::to_string(z),
                           lc.at(i), i);
            if (dff_enable)
                set_config(ti, config.at(y).at(x), "NegClk", neg_clk);
        } else if (cell.second->type == "SB_IO") {
            TileInfoPOD &ti = bi.tiles_nonrouting[TILE_IO];
            unsigned pin_type = get_param_or_def(cell.second, "PIN_TYPE");
            bool neg_trigger = get_param_or_def(cell.second, "NEG_TRIGGER");
            bool pullup = get_param_or_def(cell.second, "PULLUP");
            for (int i = 0; i < 6; i++) {
                bool val = (pin_type >> i) & 0x01;
                set_config(ti, config.at(y).at(x),
                           "IOB_" + std::to_string(z) + ".PINTYPE_" +
                                   std::to_string(i),
                           val);
            }

            auto ieren = get_ieren(bi, x, y, z);
            int iex, iey, iez;
            std::tie(iex, iey, iez) = ieren;
            assert(iez != -1);

            bool input_en = false;
            if ((chip.wire_to_net[chip.getWireBelPin(bel, PIN_D_IN_0).index] !=
                 IdString()) ||
                (chip.wire_to_net[chip.getWireBelPin(bel, PIN_D_IN_1).index] !=
                 IdString())) {
                input_en = true;
            }

            if (chip.args.type == ChipArgs::LP1K ||
                chip.args.type == ChipArgs::HX1K) {
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.IE_" + std::to_string(iez), !input_en);
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.REN_" + std::to_string(iez), !pullup);
            } else {
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.IE_" + std::to_string(iez), input_en);
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.REN_" + std::to_string(iez), !pullup);
            }
        } else if (cell.second->type == "SB_GB") {
            // no cell config bits
        } else if (cell.second->type == "ICESTORM_RAM") {
            const BelInfoPOD &beli = ci.bel_data[bel.index];
            int x = beli.x, y = beli.y;
            const TileInfoPOD &ti_ramt = bi.tiles_nonrouting[TILE_RAMT];
            const TileInfoPOD &ti_ramb = bi.tiles_nonrouting[TILE_RAMB];
            if (!(chip.args.type == ChipArgs::LP1K ||
                  chip.args.type == ChipArgs::HX1K)) {
                set_config(ti_ramb, config.at(y).at(x), "RamConfig.PowerUp",
                           true);
            }
            bool negclk_r = get_param_or_def(cell.second, "NEG_CLK_R");
            bool negclk_w = get_param_or_def(cell.second, "NEG_CLK_W");
            int write_mode = get_param_or_def(cell.second, "WRITE_MODE");
            int read_mode = get_param_or_def(cell.second, "READ_MODE");
            set_config(ti_ramb, config.at(y).at(x), "NegClk", negclk_w);
            set_config(ti_ramt, config.at(y + 1).at(x), "NegClk", negclk_r);

            set_config(ti_ramt, config.at(y + 1).at(x), "RamConfig.CBIT_0",
                       write_mode & 0x1);
            set_config(ti_ramt, config.at(y + 1).at(x), "RamConfig.CBIT_1",
                       write_mode & 0x2);
            set_config(ti_ramt, config.at(y + 1).at(x), "RamConfig.CBIT_2",
                       read_mode & 0x1);
            set_config(ti_ramt, config.at(y + 1).at(x), "RamConfig.CBIT_3",
                       read_mode & 0x2);

        } else {
            assert(false);
        }
    }
    // Set config bits in unused IO and RAM
    for (auto bel : chip.getBels()) {
        if (chip.bel_to_cell[bel.index] == IdString() &&
            chip.getBelType(bel) == TYPE_SB_IO) {
            TileInfoPOD &ti = bi.tiles_nonrouting[TILE_IO];
            const BelInfoPOD &beli = ci.bel_data[bel.index];
            int x = beli.x, y = beli.y, z = beli.z;
            auto ieren = get_ieren(bi, x, y, z);
            int iex, iey, iez;
            std::tie(iex, iey, iez) = ieren;
            if (iez != -1) {
                if (chip.args.type == ChipArgs::LP1K ||
                    chip.args.type == ChipArgs::HX1K) {
                    set_config(ti, config.at(iey).at(iex),
                               "IoCtrl.IE_" + std::to_string(iez), true);
                    set_config(ti, config.at(iey).at(iex),
                               "IoCtrl.REN_" + std::to_string(iez), false);
                }
            }
        } else if (chip.bel_to_cell[bel.index] == IdString() &&
                   chip.getBelType(bel) == TYPE_ICESTORM_RAM) {
            const BelInfoPOD &beli = ci.bel_data[bel.index];
            int x = beli.x, y = beli.y;
            TileInfoPOD &ti = bi.tiles_nonrouting[TILE_RAMB];
            if ((chip.args.type == ChipArgs::LP1K ||
                 chip.args.type == ChipArgs::HX1K)) {
                set_config(ti, config.at(y).at(x), "RamConfig.PowerUp", true);
            }
        }
    }

    // Set other config bits
    for (int y = 0; y < ci.height; y++) {
        for (int x = 0; x < ci.width; x++) {
            TileType tile = tile_at(chip, x, y);
            TileInfoPOD &ti = bi.tiles_nonrouting[tile];

            // set all ColBufCtrl bits (FIXME)
            bool setColBufCtrl = true;
            if (chip.args.type == ChipArgs::LP1K ||
                chip.args.type == ChipArgs::HX1K) {
                if (tile == TILE_RAMB || tile == TILE_RAMT) {
                    setColBufCtrl = (y == 3 || y == 5 || y == 11 || y == 13);
                } else {
                    setColBufCtrl = (y == 4 || y == 5 || y == 12 || y == 13);
                }
            } else if (chip.args.type == ChipArgs::LP8K ||
                       chip.args.type == ChipArgs::HX8K) {
                setColBufCtrl = (y == 8 || y == 9 || y == 24 || y == 25);
            } else if (chip.args.type == ChipArgs::UP5K) {
                if (tile == TILE_LOGIC) {
                    setColBufCtrl = (y == 4 || y == 5 || y == 14 || y == 15 ||
                                     y == 26 || y == 27);
                } else {
                    setColBufCtrl = false;
                }
            }
            if (setColBufCtrl) {
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_0",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_1",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_2",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_3",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_4",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_5",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_6",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_7",
                           true);
            }
        }
    }

    // Write config out
    for (int y = 0; y < ci.height; y++) {
        for (int x = 0; x < ci.width; x++) {
            TileType tile = tile_at(chip, x, y);
            if (tile == TILE_NONE)
                continue;
            switch (tile) {
            case TILE_LOGIC:
                out << ".logic_tile";
                break;
            case TILE_IO:
                out << ".io_tile";
                break;
            case TILE_RAMB:
                out << ".ramb_tile";
                break;
            case TILE_RAMT:
                out << ".ramt_tile";
                break;
            default:
                assert(false);
            }
            out << " " << x << " " << y << std::endl;
            for (auto row : config.at(y).at(x)) {
                for (auto col : row) {
                    if (col == 1)
                        out << "1";
                    else
                        out << "0";
                }
                out << std::endl;
            }
            out << std::endl;
        }
    }

    // Write RAM init data
    for (auto cell : design.cells) {
        if (cell.second->bel != BelId()) {
            if (cell.second->type == "ICESTORM_RAM") {
                const BelInfoPOD &beli = ci.bel_data[cell.second->bel.index];
                int x = beli.x, y = beli.y;
                out << ".ram_data " << x << " " << y << std::endl;
                for (int w = 0; w < 16; w++) {
                    std::vector<bool> bits(256);
                    std::string init = get_param_str_or_def(
                            cell.second,
                            std::string("INIT_") + get_hexdigit(w));
                    assert(init != "");
                    for (int i = 0; i < init.size(); i++) {
                        bool val = (init.at((init.size() - 1) - i) == '1');
                        bits.at(i) = val;
                    }
                    for (int i = bits.size() - 4; i >= 0; i -= 4) {
                        int c = bits.at(i) + (bits.at(i + 1) << 1) +
                                (bits.at(i + 2) << 2) + (bits.at(i + 3) << 3);
                        out << char(std::tolower(get_hexdigit(c)));
                    }
                    out << std::endl;
                }
                out << std::endl;
            }
        }
    }

    // Write symbols
    const bool write_symbols = 1;
    for (auto wire : chip.getWires()) {
        IdString net = chip.getWireNet(wire, false);
        if (net != IdString())
            out << ".sym " << wire.index << " " << net << std::endl;
    }
}

NEXTPNR_NAMESPACE_END
