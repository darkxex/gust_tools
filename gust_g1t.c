/*
  gust_g1t - DDS texture unpacker for Gust (Koei/Tecmo) .g1t files
  Copyright © 2019-2021 VitaSmith

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "utf8.h"
#include "util.h"
#include "parson.h"
#include "dds.h"

#define JSON_VERSION            1
#define G1TG_LE_MAGIC           0x47315447  // 'GT1G'
#define G1TG_BE_MAGIC           0x47543147  // 'G1TG'

// G1T texture flags
#define G1T_FLAG_SRGB           0x0002000000ULL // Not sure if this one is correct...
#define G1T_FLAG_LOCAL_XDATA    0x0010000000ULL // Set if the texture has local extra data in the texture entry.
#define G1T_FLAG_IS_NORMAL_MAP  0x1000000000ULL // Set if the texture is a normal map.

// Known platforms
#define SONY_PS2                0x00
#define SONY_PS3                0x01
#define MICROSOFT_X360          0x02
#define NINTENDO_WII            0x03
#define NINTENDO_DS             0x04
#define NINTENDO_3DS            0x05
#define SONY_PSV                0x06
#define GOOGLE_ANDROID          0x07
#define APPLE_IOS               0x08
#define NINTENDO_WIIU           0x09
#define MICROSOFT_WINDOWS       0x0A
#define SONY_PS4                0x0B
#define MICROSOFT_XONE          0x0C
#define NINTENDO_SWITCH         0x10

#pragma pack(push, 1)
typedef struct {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    total_size;
    uint32_t    header_size;
    uint32_t    nb_textures;
    uint32_t    platform;       // See the platforms list above
    uint32_t    extra_size;
} g1t_header;

// This is (optionally) followed by uint32_t extra_flags[nb_textures]
// This is followed by uint32_t offset[nb_textures]
// This is (optionally) followed by an array of global extra data (for textures with G1T_FLAG_GLOBAL_XDATA)

typedef struct {
    uint8_t     exts : 4;       // Set to 1 for extended textures
    uint8_t     mipmaps : 4;
    uint8_t     type;
    uint8_t     dx : 4;
    uint8_t     dy : 4;
    // The flags appear to be a set of nibbles (that need to be reordered
    // for Big Endian) that occupy 5 bytes in all.
    uint8_t     hflags;         // high part
    uint32_t    lflags;         // low part
} g1t_tex_header;

// May be followed by extra_data[]

#pragma pack(pop)

// Same order as enum DDS_FORMAT
const char* argb_name[] = { NULL, NULL, "ABGR", "ARGB", "GRAB", "RGBA" };

static size_t write_dds_header(FILE* fd, enum DDS_FORMAT format, uint32_t width,
                               uint32_t height, uint32_t mipmaps, uint64_t flags)
{
    if ((fd == NULL) || (width == 0) || (height == 0))
        return 0;

    DDS_HEADER header = { 0 };
    uint32_t bpp = dds_bpp(format);
    header.size = 124;
    header.flags = DDS_HEADER_FLAGS_TEXTURE;
    header.height = height;
    header.width = width;
    header.ddspf.size = 32;
    if (format == DDS_FORMAT_BGR) {
        header.ddspf.flags = DDS_RGB;
        header.ddspf.RGBBitCount = bpp;
        switch (bpp) {
        case 24:
            header.ddspf.RBitMask = 0x00ff0000;
            header.ddspf.GBitMask = 0x0000ff00;
            header.ddspf.BBitMask = 0x000000ff;
            break;
        default:
            fprintf(stderr, "ERROR: Unsupported bits-per-pixel value %d\n", bpp);
            return 0;
        }
    } else if (format >= DDS_FORMAT_ABGR && format <= DDS_FORMAT_RGBA) {
        header.ddspf.flags = DDS_RGBA;
        header.ddspf.RGBBitCount = bpp;
        // Always save as ARGB, to keep VS and PhotoShop happy
        switch (bpp) {
        case 32:
            header.ddspf.RBitMask = 0x00ff0000;
            header.ddspf.GBitMask = 0x0000ff00;
            header.ddspf.BBitMask = 0x000000ff;
            header.ddspf.ABitMask = 0xff000000;
            break;
        // I have absolutely no idea if the following will work...
        case 64:
            header.ddspf.RBitMask = 0x0000ffff;
            header.ddspf.GBitMask = 0xffff0000;
            header.ddspf.BBitMask = 0x0000ffff;
            header.ddspf.ABitMask = 0xffff0000;
            break;
        case 128:
            header.ddspf.RBitMask = 0xffffffff;
            header.ddspf.GBitMask = 0xffffffff;
            header.ddspf.BBitMask = 0xffffffff;
            header.ddspf.ABitMask = 0xffffffff;
            break;
        default:
            fprintf(stderr, "ERROR: Unsupported bits-per-pixel value %d\n", bpp);
            return 0;
        }
    } else if (format == DDS_FORMAT_R) {
        header.ddspf.flags = DDS_RGBA;
        header.ddspf.RGBBitCount = bpp;
        header.ddspf.RBitMask = (uint32_t)((1ULL << bpp) - 1);
    } else {
        header.ddspf.flags = DDS_FOURCC;
        header.ddspf.fourCC = get_fourCC(format);
    }
    header.caps = DDS_SURFACE_FLAGS_TEXTURE;
    if (mipmaps != 0) {
        header.mipMapCount = mipmaps;
        header.flags |= DDS_HEADER_FLAGS_MIPMAP;
        header.caps |= DDS_SURFACE_FLAGS_MIPMAP;
    }
    if (flags & G1T_FLAG_IS_NORMAL_MAP)
        header.ddspf.flags |= DDS_NORMAL;
    size_t r = fwrite(&header, sizeof(DDS_HEADER), 1, fd);
    if (r != 1)
        return r;
    if (format == DDS_FORMAT_BC7) {
        DDS_HEADER_DXT10 dxt10_hdr = { 0 };
        dxt10_hdr.dxgiFormat = (flags & G1T_FLAG_SRGB) ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        dxt10_hdr.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
        dxt10_hdr.miscFlags2 = DDS_ALPHA_MODE_STRAIGHT;
        dxt10_hdr.arraySize = 1; // Must be set to 1 for 3D texture
        r = fwrite(&dxt10_hdr, sizeof(DDS_HEADER_DXT10), 1, fd);
    }
    return r;
}

static void rgba_convert(const enum DDS_FORMAT format, const char* in,
                         const char* out, uint8_t* buf, const uint32_t size)
{
    uint32_t bits_per_pixel = dds_bpp(format);
    assert(bits_per_pixel % 8 == 0);
    assert(format >= DDS_FORMAT_BGR && format <= DDS_FORMAT_RGBA);

    const int rgba[4] = { 'R', 'G', 'B', 'A' };
    if (strcmp(in, out) == 0)
        return;

    uint32_t mask[4];
    int rot[4];
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t pos_in = 3 - (uint32_t)((uintptr_t)strchr(in, rgba[i]) - (uintptr_t)in);
        uint32_t pos_out = 3 - (uint32_t)((uintptr_t)strchr(out, rgba[i]) - (uintptr_t)out);
        mask[i] = ((1 << bits_per_pixel / 4) - 1) << (pos_in * 8);
        rot[i] = (pos_out - pos_in) * 8;
    }

    for (uint32_t j = 0; j < size; j += 4) {
        uint32_t s;
        switch (bits_per_pixel) {
        case 16: s = getbe16(&buf[j]); break;
        case 24: s = getbe24(&buf[j]); break;
        default: s = getbe32(&buf[j]); break;
        }
        uint32_t d = 0;
        for (uint32_t i = 0; i < 4; i++)
            d |= (rot[i] > 0) ? ((s & mask[i]) << rot[i]) : ((s & mask[i]) >> -rot[i]);
        switch (bits_per_pixel) {
        case 16: setbe16(&buf[j], (uint16_t)d); break;
        case 24: setbe24(&buf[j], d); break;
        default: setbe32(&buf[j], d); break;
        }
    }
}

// "Inflate" a 32 bit value by interleaving 0 bits at odd positions.
static __inline uint32_t inflate_bits(uint32_t x)
{
    x &= 0x0000FFFF;
    x = (x | (x << 8))  & 0x00FF00FF;
    x = (x | (x << 4))  & 0x0F0F0F0F;
    x = (x | (x << 2))  & 0x33333333;
    x = (x | (x << 1))  & 0x55555555;
    return x;
}

// "Deflate" a 32-bit value by deinterleaving all odd bits.
static __inline uint32_t deflate_bits(uint32_t x)
{
    x &= 0x55555555;
    x = (x | (x >> 1))  & 0x33333333;
    x = (x | (x >> 2))  & 0x0F0F0F0F;
    x = (x | (x >> 4))  & 0x00FF00FF;
    x = (x | (x >> 8))  & 0x0000FFFF;
    return x;
}

// From two 32-bit (x,y) coordinates, compute the 64-bit Morton (or Z-order) value.
static __inline uint32_t xy_to_morton(uint32_t x, uint32_t y)
{
    return (inflate_bits(x) << 1) | (inflate_bits(y) << 0);
}

// Fom a 32-bit Morton (Z-order) value, recover two 32-bit (x,y) coordinates.
static __inline void morton_to_xy(uint32_t z, uint32_t* x, uint32_t* y)
{
    *x = deflate_bits(z >> 1);
    *y = deflate_bits(z >> 0);
}

// Apply or reverse a Morton transformation, a.k.a. a Z-order curve, to a texture.
// If morton_order is negative, a reverse Morton transformation is applied.
static void mortonize(const enum DDS_FORMAT format, const int16_t morton_order,
                      uint32_t width, uint32_t height, uint8_t* buf, const uint32_t size, uint32_t wf)
{
    const uint32_t bits_per_element = dds_bpp(format) * dds_bwh(format) * dds_bwh(format) * wf;
    const uint32_t bytes_per_element = bits_per_element / 8;
    width /= dds_bwh(format) * wf;
    height /= dds_bwh(format);
    uint32_t num_elements = size / bytes_per_element;
    uint16_t k = (uint16_t)abs(morton_order);
    bool reverse = (morton_order != (int16_t)k);

    // Only deal with elements that are multiple of one byte in size
    assert(bits_per_element % 8 == 0);
    // Validate that the size of the buffer matches the dimensions provided
    assert(bytes_per_element * width * height == size);
    // Only deal with texture that are smaller than 64k*64k
    assert((width < 0x10000) && (height < 0x10000));
    // Ensure that width and height are an exact multiple of 2^k
    assert(width % (1 << k) == 0);
    assert(height % (1 << k) == 0);
    // Ensure that we won't produce x or y that are larger than the maximum dimension
    assert(k <= log2(max(width, height)));
    uint32_t tile_width = 1 << k;
    uint32_t tile_size = tile_width * tile_width;
    uint32_t mask = tile_size - 1;
    uint8_t* tmp_buf = (uint8_t*)malloc(size);
    for (uint32_t i = 0; i < num_elements; i++) {
        uint32_t j, x, y;
        if (reverse) {  // Morton value to an (x,y) pair
            // Recover (x,y) for the Morton tile
            morton_to_xy(i & mask, &x, &y);
            // Now apply untiling by offsetting (x,y) with the tile positiom
            x += ((i / tile_size) % (width / tile_width)) * tile_width;
            y += ((i / tile_size) / (width / tile_width)) * tile_width;
            j = y * width + x;
        } else {        // Morton value from an (x,y) pair
            x = i % width; y = i / width;
            j = xy_to_morton(x, y) & mask;
            // Now, apply tiling. This is accomplished by offseting our value
            // with the current tile position multiplied by the tile size.
            j += ((y / tile_width) * (width / tile_width) + (x / tile_width)) * tile_size;
        }
        assert(j < num_elements);
        memcpy(&tmp_buf[j * bytes_per_element], &buf[i * bytes_per_element], bytes_per_element);
    }
    memcpy(buf, tmp_buf, size);
    free(tmp_buf);
}

static void flip(uint32_t bits_per_pixel, uint8_t* buf, const uint32_t size, uint32_t width)
{
    assert(bits_per_pixel % 8 == 0);
    const uint32_t line_size = width * (bits_per_pixel / 8);
    assert(size % line_size == 0);
    const uint32_t max_line = (size / line_size) - 1;

    uint8_t* tmp_buf = (uint8_t*)malloc(size);

    for (uint32_t i = 0; i <= max_line; i++)
        memcpy(&tmp_buf[i * line_size], &buf[(max_line - i) * line_size], line_size);

    memcpy(buf, tmp_buf, size);
    free(tmp_buf);
}

int main_utf8(int argc, char** argv)
{
    int r = -1;
    FILE *file = NULL;
    uint8_t* buf = NULL;
    uint32_t* offset_table = NULL;
    uint32_t magic;
    char path[256], *dir = NULL;
    JSON_Value* json = NULL;
    bool list_only = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'l');
    bool flip_image = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'f');
    bool no_prompt = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'y');

    if ((argc != 2) && !list_only && !flip_image && !no_prompt) {
        printf("%s %s (c) 2019-2021 VitaSmith\n\n"
            "Usage: %s [-l] [-f] [-y] <file or directory>\n\n"
            "Extracts (file) or recreates (directory) a Gust .g1t texture archive.\n\n"
            "Note: A backup (.bak) of the original is automatically created, when the target\n"
            "is being overwritten for the first time.\n",
            _appname(argv[0]), GUST_TOOLS_VERSION_STR, _appname(argv[0]));
        return 0;
    }

    if (is_directory(argv[argc - 1])) {
        if (list_only) {
            fprintf(stderr, "ERROR: Option -l is not supported when creating an archive\n");
            goto out;
        }
        snprintf(path, sizeof(path), "%s%cg1t.json", argv[argc - 1], PATH_SEP);
        if (!is_file(path)) {
            fprintf(stderr, "ERROR: '%s' does not exist\n", path);
            goto out;
        }
        json = json_parse_file_with_comments(path);
        if (json == NULL) {
            fprintf(stderr, "ERROR: Can't parse JSON data from '%s'\n", path);
            goto out;
        }
        const uint32_t json_version = json_object_get_uint32(json_object(json), "json_version");
        if (json_version != JSON_VERSION) {
            fprintf(stderr, "ERROR: This utility is not compatible with the JSON file provided.\n"
                "You need to (re)extract the '.g1t' using this application.\n");
            goto out;
        }
        const char* filename = json_object_get_string(json_object(json), "name");
        const char* version = json_object_get_string(json_object(json), "version");
        if ((filename == NULL) || (version == NULL))
            goto out;
        if (json_object_get_uint32(json_object(json), "platform") == SONY_PS3)
            data_endianness = big_endian;
        if (!flip_image)
            flip_image = json_object_get_boolean(json_object(json), "flip");
        strcpy(path, argv[argc - 1]);
        if (get_trailing_slash(path) != 0)
            path[get_trailing_slash(path)] = 0;
        else
            path[0] = 0;
        strcat(path, filename);
        path[sizeof(path) - 1] = 0;
        printf("Creating '%s'...\n", path);
        create_backup(path);
        file = fopen_utf8(path, "wb+");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't create file '%s'\n", path);
            goto out;
        }
        g1t_header hdr = { 0 };
        hdr.magic = (data_endianness == little_endian) ? G1TG_LE_MAGIC : G1TG_BE_MAGIC;
        hdr.version = getv32(getbe32(version));
        hdr.total_size = 0;  // To be rewritten when we're done
        hdr.nb_textures = getv32(json_object_get_uint32(json_object(json), "nb_textures"));
        hdr.platform = getv32(json_object_get_uint32(json_object(json), "platform"));
        hdr.extra_size = getv32(json_object_get_uint32(json_object(json), "extra_size"));
        hdr.header_size = getv32(sizeof(hdr) + getv32(hdr.nb_textures) * sizeof(uint32_t));
        if (fwrite(&hdr, sizeof(hdr), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't write header\n");
            goto out;
        }

        JSON_Array* json_extra_flags_array = json_object_get_array(json_object(json), "extra_flags");
        if (json_array_get_count(json_extra_flags_array) != getv32(hdr.nb_textures)) {
            fprintf(stderr, "ERROR: number of extra flags doesn't match number of textures\n");
            goto out;
        }
        for (uint32_t i = 0; i < getv32(hdr.nb_textures); i++) {
            uint32_t extra_flag = getv32((uint32_t)json_array_get_number(json_extra_flags_array, i));
            if (fwrite(&extra_flag, sizeof(uint32_t), 1, file) != 1) {
                fprintf(stderr, "ERROR: Can't write extra flags\n");
                goto out;
            }
        }

        offset_table = calloc(getv32(hdr.nb_textures), sizeof(uint32_t));
        offset_table[0] = getv32(hdr.nb_textures) * sizeof(uint32_t);
        if (fwrite(offset_table, getv32(hdr.nb_textures) * sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't write texture offsets\n");
            goto out;
        }

        // Deal with the global extra data array
        if (hdr.extra_size != 0) {
            JSON_Array* json_global_extra_data_array = json_object_get_array(json_object(json), "extra_data");
            for (uint32_t i = 0; i < hdr.extra_size / sizeof(uint32_t); i++) {
                uint32_t extra_data = getv32((uint32_t)json_array_get_number(json_global_extra_data_array, i));
                if (fwrite(&extra_data, sizeof(uint32_t), 1, file) != 1) {
                    fprintf(stderr, "ERROR: Can't write global extra data\n");
                    goto out;
                }
            }
        }

        JSON_Array* textures_array = json_object_get_array(json_object(json), "textures");
        if (json_array_get_count(textures_array) != getv32(hdr.nb_textures)) {
            fprintf(stderr, "ERROR: number of textures in array doesn't match\n");
            goto out;
        }

        printf("TYPE OFFSET     SIZE       NAME");
        dir = strdup(argv[argc - 1]);
        if (dir == NULL) {
            fprintf(stderr, "ERROR: Alloc error\n");
            goto out;
        }
        dir[get_trailing_slash(dir)] = 0;
        for (size_t i = 0; i < strlen(_basename(argv[argc - 1])); i++)
            putchar(' ');
        printf("     DIMENSIONS MIPMAPS ZSTACK\n");
        for (uint32_t i = 0; i < getv32(hdr.nb_textures); i++) {
            offset_table[i] = ftell(file) - getv32(hdr.header_size);
            JSON_Object* texture_entry = json_array_get_object(textures_array, i);
            g1t_tex_header tex = { 0 };
            tex.type = json_object_get_uint8(texture_entry, "type");
            tex.exts = json_object_get_uint8(texture_entry, "exts");
            uint64_t flags = json_object_get_uint64(texture_entry, "flags");
            uint32_t z_stack = json_object_get_uint32(texture_entry, "z_stack");
            if (z_stack == 0)
                z_stack = 1;
            tex.hflags = (uint8_t)(flags >> 32);
            tex.lflags = (uint32_t)flags;
            // Read the DDS file
            snprintf(path, sizeof(path), "%s%s%c%s", dir, _basename(argv[argc - 1]), PATH_SEP,
                json_object_get_string(texture_entry, "name"));
            uint32_t texture_size = read_file(path, &buf);
            if (texture_size == UINT32_MAX)
                goto out;
            if (texture_size <= sizeof(DDS_HEADER)) {
                fprintf(stderr, "ERROR: '%s' is too small\n", path);
                goto out;
            }
            if (*((uint32_t*)buf) != DDS_MAGIC) {
                fprintf(stderr, "ERROR: '%s' is not a DDS file\n", path);
                goto out;
            }
            DDS_HEADER* dds_header = (DDS_HEADER*)&buf[sizeof(uint32_t)];
            texture_size -= sizeof(uint32_t) + sizeof(DDS_HEADER);
            uint8_t* dds_payload = (uint8_t*)&buf[sizeof(uint32_t) + sizeof(DDS_HEADER)];
            // Adjust the height if we have a Z Stack
            dds_header->height /= z_stack;
            // We may have a DXT10 additional header
            if (dds_header->ddspf.fourCC == get_fourCC(DDS_FORMAT_DX10)) {
                texture_size -= sizeof(DDS_HEADER_DXT10);
                dds_payload = &dds_payload[sizeof(DDS_HEADER_DXT10)];
            }
            tex.mipmaps = json_object_get_uint8(texture_entry, "mipmaps");
            if (tex.mipmaps == 0) {
                tex.mipmaps = (uint8_t)dds_header->mipMapCount;
            } else if ((uint8_t)dds_header->mipMapCount < tex.mipmaps) {
                fprintf(stderr, "WARNING: Number of mipmaps from imported texture is smaller than original\n");
                tex.mipmaps = (uint8_t)dds_header->mipMapCount;
            } else if ((uint8_t)dds_header->mipMapCount > tex.mipmaps) {
                fprintf(stderr, "NOTE: Truncating number of mipmaps from %d to %d\n", dds_header->mipMapCount, tex.mipmaps);
            }
            // Are both width and height a power of two?
            bool po2_sizes = is_power_of_2(dds_header->width) && is_power_of_2(dds_header->height);
            if (po2_sizes) {
                tex.dx = (uint8_t)find_msb(dds_header->width);
                tex.dy = (uint8_t)find_msb(dds_header->height);
            }
            if (data_endianness != platform_endianness) {
                uint8_t swap_tmp = tex.dx;
                tex.dx = tex.dy;
                tex.dy = swap_tmp;
                swap_tmp = tex.exts;
                tex.exts = tex.mipmaps;
                tex.mipmaps = swap_tmp;
                tex.hflags = ((tex.hflags & 0xf0) >> 4) | ((tex.hflags & 0x0f) << 4);
                tex.lflags = ((tex.lflags & 0xf0f0f0f0) >> 4) | ((tex.lflags & 0x0f0f0f0f) << 4);
            }
            // Write texture header
            if (fwrite(&tex, sizeof(tex), 1, file) != 1) {
                fprintf(stderr, "ERROR: Can't write texture header\n");
                goto out;
            }
            // Swap back tex.mipmaps for the rest of our processing
            if (data_endianness != platform_endianness) {
                uint8_t swap_tmp = tex.exts;
                tex.exts = tex.mipmaps;
                tex.mipmaps = swap_tmp;
            }
            // Write extra data
            if (flags & G1T_FLAG_LOCAL_XDATA) {
                JSON_Array* json_local_extra_data_array = json_object_get_array(texture_entry, "extra_data");
                uint32_t extra_data_size = (uint32_t)(json_array_get_count(json_local_extra_data_array) + 1) * sizeof(uint32_t);
                if (!po2_sizes && extra_data_size < 4 * sizeof(uint32_t)) {
                    fprintf(stderr, "ERROR: Non power-of-two width or height is missing from extra data\n");
                    goto out;
                }
                extra_data_size = getv32(extra_data_size);
                if (fwrite(&extra_data_size, sizeof(uint32_t), 1, file) != 1) {
                    fprintf(stderr, "ERROR: Can't write extra data size\n");
                    goto out;
                }
                for (size_t j = 0; j < json_array_get_count(json_local_extra_data_array); j++) {
                    uint32_t extra_data = (uint32_t)json_array_get_number(json_local_extra_data_array, j);
                    if ((j == 2) && (!po2_sizes) && (extra_data != dds_header->width)) {
                        fprintf(stderr, "ERROR: DDS width and local extra data width don't match\n");
                        goto out;
                    }
                    if ((j == 3) && (!po2_sizes) && (extra_data != dds_header->height)) {
                        fprintf(stderr, "ERROR: DDS height and local extra data height don't match\n");
                        goto out;
                    }
                    extra_data = getv32(extra_data);
                    if (fwrite(&extra_data, sizeof(uint32_t), 1, file) != 1) {
                        fprintf(stderr, "ERROR: Can't write local extra data\n");
                        goto out;
                    }
                }
            }

            // Set the default ARGB format for the platform
            uint32_t default_texture_format;
            switch (getv32(hdr.platform)) {
            case NINTENDO_DS:
            case NINTENDO_3DS:
            case SONY_PS4:
                default_texture_format = DDS_FORMAT_GRAB;
                break;
            case SONY_PSV:
            case NINTENDO_SWITCH:
                default_texture_format = DDS_FORMAT_ARGB;
                break;
            default:    // PC and other platforms
                default_texture_format = DDS_FORMAT_RGBA;
                break;
            }
            uint32_t texture_format = default_texture_format;
            bool swizzled = false;
            switch (tex.type) {
            case 0x00: break;
            case 0x01: break;
            case 0x02: break;
//            case 0x03: bpp = 64; break;
//            case 0x04: bpp = 128; break;
            case 0x06: texture_format = DDS_FORMAT_DXT1; break;
//            case 0x07: texture_format = DDS_FORMAT_DXT3; break;
            case 0x08: texture_format = DDS_FORMAT_DXT5; break;
            case 0x09: swizzled = true; break;
//            case 0x0A: swizzled = true; break;
            case 0x10: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;
            case 0x12: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;
            case 0x21: break;
            case 0x3C: texture_format = DDS_FORMAT_DXT1; break;
            case 0x3D: texture_format = DDS_FORMAT_DXT1; break;
            case 0x45: texture_format = DDS_FORMAT_BGR; swizzled = true; break;
            case 0x59: texture_format = DDS_FORMAT_DXT1; break;
            case 0x5B: texture_format = DDS_FORMAT_DXT5; break;
            case 0x5C: texture_format = DDS_FORMAT_BC4; break;
//            case 0x5D: texture_format = DDS_FORMAT_ATI1; break;
//            case 0x5E: texture_format = DDS_FORMAT_ATI2; break;
            case 0x5F: texture_format = DDS_FORMAT_BC7; break;
            case 0x60: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;
            case 0x62: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;
//            case 0x63: texture_format = DDS_FORMAT_BC4; swizzled = true; break;
//            case 0x64: texture_format = DDS_FORMAT_BC5; swizzled = true; break;
//            case 0x65: texture_format = DDS_FORMAT_BC6; swizzled = true; break;
//            case 0x66: texture_format = DDS_FORMAT_BC7; swizzled = true; break;
//            case 0x72: texture_format = DDS_FORMAT_????; break;
            default:
                fprintf(stderr, "ERROR: Unsupported texture type 0x%02x\n", tex.type);
                goto out;
            }

            uint32_t expected_texture_size = 0;
            for (int j = 0; j < tex.mipmaps; j++)
                expected_texture_size += MIPMAP_SIZE(texture_format, j, dds_header->width, dds_header->height);
            expected_texture_size *= z_stack;
            if (expected_texture_size > texture_size) {
                fprintf(stderr, "ERROR: expected_texture_size %8x > %8x\n", expected_texture_size, texture_size);
                goto out;
            }
            if ((texture_size * 8) % dds_bpp(texture_format) != 0) {
                fprintf(stderr, "ERROR: Texture size should be a multiple of %d bits\n", dds_bpp(texture_format));
                goto out;
            }
            if (expected_texture_size < texture_size) {
                // Only display the warning if we aren't truncating mipmaps
                if ((uint8_t)dds_header->mipMapCount <= tex.mipmaps)
                    fprintf(stderr, "WARNING: Reducing texture size\n");
                texture_size = expected_texture_size;
            }

            switch (dds_header->ddspf.flags & (DDS_ALPHAPIXELS | DDS_FOURCC | DDS_RGB)) {
            case DDS_RGBA:
                if ((dds_header->ddspf.RGBBitCount != 32) && (dds_header->ddspf.RGBBitCount != 64) &&
                    (dds_header->ddspf.RGBBitCount != 128)) {
                    fprintf(stderr, "ERROR: '%s' is not an ARGB texture we support\n", path);
                    goto out;
                }
                break;
            case DDS_RGB:
                if ((dds_header->ddspf.RGBBitCount != 24) ||
                    (dds_header->ddspf.RBitMask != 0x00ff0000) || (dds_header->ddspf.GBitMask != 0x0000ff00) ||
                    (dds_header->ddspf.BBitMask != 0x000000ff) || (dds_header->ddspf.ABitMask != 0x00000000)) {
                    fprintf(stderr, "ERROR: '%s' is not an RGB texture we support\n", path);
                    goto out;
                }
            case DDS_FOURCC:
                break;
            default:
                fprintf(stderr, "ERROR: '%s' is not a texture we support\n", path);
                goto out;
            }

            if (flip_image || ((getv32(hdr.platform) == NINTENDO_3DS) && (tex.type == 0x09 || tex.type == 0x45)))
                flip(dds_bpp(texture_format), dds_payload, texture_size, dds_header->width);

            if (swizzled) {
                int16_t mo = 0;     // Morton order
                uint32_t wf = 1;    // Width factor
                switch (getv32(hdr.platform)) {
                case SONY_PS4:
                case NINTENDO_3DS:
                    mo = 3;
                    wf = 2;
                    break;
                case NINTENDO_WIIU:
                    wf = 2;
                    // Fall through
                default:
                    mo = (int16_t)log2(min(dds_header->width / dds_bwh(texture_format) / wf,
                            dds_header->height / dds_bwh(texture_format)));
                    break;
                }
                uint32_t offset = 0;
                assert(mo != 0);
                for (int j = 1; j <= tex.mipmaps && mo != 0; j++) {
                    uint32_t mipmap_size = MIPMAP_SIZE(texture_format, j - 1, dds_header->width, dds_header->height);
                    mortonize(texture_format, mo, dds_header->width / (1 << (j - 1)), dds_header->height / (1 << (j - 1)),
                        &dds_payload[offset], mipmap_size, wf);
                    offset += mipmap_size;
                    mo += (mo > 0) ? -1 : +1;
                }
            }
            if (texture_format >= DDS_FORMAT_ABGR && texture_format <= DDS_FORMAT_RGBA)
                rgba_convert(texture_format, "ARGB", argb_name[texture_format], dds_payload, texture_size);

            // Write texture
            if (fwrite(dds_payload, 1, texture_size, file) != texture_size) {
                fprintf(stderr, "ERROR: Can't write texture data\n");
                goto out;
            }
            char dims[16];
            snprintf(dims, sizeof(dims), "%dx%d", dds_header->width, dds_header->height);
            printf("0x%02x 0x%08x 0x%08x %s %-10s %-7d %-6d\n", tex.type, getv32(hdr.header_size) + offset_table[i],
                (uint32_t)ftell(file) - offset_table[i] - getv32(hdr.header_size) - (uint32_t)sizeof(g1t_tex_header), path,
                dims, tex.mipmaps, z_stack);
            free(buf);
            buf = NULL;
        }
        // Update total size
        uint32_t total_size = getv32(ftell(file));
        fseek(file, 2 * sizeof(uint32_t), SEEK_SET);
        if (fwrite(&total_size, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't update total size\n");
            goto out;
        }
        // Update offset table
        fseek(file, sizeof(hdr) + getv32(hdr.nb_textures) * sizeof(uint32_t), SEEK_SET);
        for (uint32_t i = 0; i < getv32(hdr.nb_textures); i++)
            offset_table[i] = getv32(offset_table[i]);
        if (fwrite(offset_table, sizeof(uint32_t), getv32(hdr.nb_textures), file) != getv32(hdr.nb_textures)) {
            fprintf(stderr, "ERROR: Can't update texture offsets\n");
            goto out;
        }
        r = 0;
    } else {
        printf("%s '%s'...\n", list_only ? "Listing" : "Extracting", argv[argc - 1]);
        size_t len = strlen(argv[argc - 1]);
        if ((len < 4) || (argv[argc - 1][len - 4] != '.') || (argv[argc - 1][len - 3] != 'g') ||
            ((argv[argc - 1][len - 2] != '1') && (argv[argc - 1][len - 2] != 't')) ||
            ((argv[argc - 1][len - 1] != '1') && (argv[argc - 1][len - 1] != 't')) ) {
            fprintf(stderr, "ERROR: File should have a '.g1t' or 'gt1' extension\n");
            goto out;
        }
        char* g1t_pos = &argv[argc - 1][len - 4];
        file = fopen_utf8(argv[argc - 1], "rb");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't open file '%s'\n", argv[argc - 1]);
            goto out;
        }

        if (fread(&magic, sizeof(magic), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't read from '%s'\n", argv[argc - 1]);
            goto out;
        }
        if ((magic != G1TG_LE_MAGIC) && (magic != G1TG_BE_MAGIC)) {
            fprintf(stderr, "ERROR: Not a G1T file (bad magic) or unsupported platform\n");
            goto out;
        }
        if (magic == G1TG_BE_MAGIC)
            data_endianness = big_endian;
        fseek(file, 0L, SEEK_END);
        uint32_t g1t_size = (uint32_t)ftell(file);
        fseek(file, 0L, SEEK_SET);

        buf = malloc(g1t_size);
        if (buf == NULL)
            goto out;
        if (fread(buf, 1, g1t_size, file) != g1t_size) {
            fprintf(stderr, "ERROR: Can't read file\n");
            goto out;
        }

        g1t_header* hdr = (g1t_header*)buf;
        if (data_endianness != platform_endianness) {
            BSWAP_UINT32(hdr->magic);
            BSWAP_UINT32(hdr->version);
            BSWAP_UINT32(hdr->total_size);
            BSWAP_UINT32(hdr->header_size);
            BSWAP_UINT32(hdr->nb_textures);
            BSWAP_UINT32(hdr->platform);
            BSWAP_UINT32(hdr->extra_size);
        }
        if (hdr->total_size != g1t_size) {
            fprintf(stderr, "ERROR: File size mismatch\n");
            goto out;
        }
        char version[5];
        setbe32(version, hdr->version);
        version[4] = 0;
        if (hdr->version >> 16 != 0x3030)
            fprintf(stderr, "WARNING: Potentially unsupported G1T version %s\n", version);
        if (hdr->extra_size % sizeof(uint32_t)) {
            fprintf(stderr, "ERROR: Can't handle G1T files with global extra data that's not a multiple of %d\n",
                (int)sizeof(uint32_t));
            goto out;
        }
        if (hdr->extra_size > 0xFFFF) {
            fprintf(stderr, "ERROR: Can't handle G1T files with more than 64 KB of global extra data\n");
            goto out;
        }

        uint32_t* x_offset_table = (uint32_t*)&buf[hdr->header_size];

        // Keep the information required to recreate the archive in a JSON file
        json = json_value_init_object();
        json_object_set_number(json_object(json), "json_version", JSON_VERSION);
        json_object_set_string(json_object(json), "name", _basename(argv[argc - 1]));
        json_object_set_string(json_object(json), "version", version);
        json_object_set_number(json_object(json), "nb_textures", hdr->nb_textures);
        json_object_set_number(json_object(json), "platform", hdr->platform);
        json_object_set_number(json_object(json), "extra_size", hdr->extra_size);
        if (flip_image)
            json_object_set_boolean(json_object(json), "flip", true);

        g1t_pos[0] = 0;
        if (!list_only && !create_path(argv[argc - 1]))
            goto out;

        JSON_Value* json_extra_flags_array = json_value_init_array();
        JSON_Value* json_global_extra_data_array = json_value_init_array();
        JSON_Value* json_textures_array = json_value_init_array();

        for (uint32_t i = 0; i < hdr->extra_size; i += sizeof(uint32_t))
            json_array_append_number(json_array(json_global_extra_data_array),
                getle32(&buf[hdr->header_size + hdr->nb_textures * sizeof(uint32_t) + i]));

        printf("TYPE OFFSET     SIZE       NAME");
        for (size_t i = 0; i < strlen(_basename(argv[argc - 1])); i++)
            putchar(' ');
        printf("     DIMENSIONS MIPMAPS ZSTACK\n");
        dir = strdup(argv[argc - 1]);
        if (dir == NULL) {
            fprintf(stderr, "ERROR: Alloc error\n");
            goto out;
        }
        dir[get_trailing_slash(dir)] = 0;

        // Set the default RGBA texture format for the platform
        uint32_t default_texture_format;
        switch (hdr->platform) {
        case NINTENDO_DS:
        case NINTENDO_3DS:
        case SONY_PS4:
            default_texture_format = DDS_FORMAT_GRAB;
            break;
        case SONY_PSV:
        case NINTENDO_SWITCH:
            default_texture_format = DDS_FORMAT_ARGB;
            break;
        default:    // PC and other platforms
            default_texture_format = DDS_FORMAT_RGBA;
            break;
        }

        r = 0;
        for (uint32_t i = 0; i < hdr->nb_textures; i++) {
            // There's an array of flags after the hdr
            json_array_append_number(json_array(json_extra_flags_array),
                getle32(&buf[(uint32_t)sizeof(g1t_header) + 4 * i]));
            uint32_t z_stack = 1, pos = hdr->header_size + getv32(x_offset_table[i]);
            g1t_tex_header* tex = (g1t_tex_header*)&buf[pos];
            if (data_endianness != platform_endianness) {
                uint8_t swap_tmp = tex->dx;
                tex->dx = tex->dy;
                tex->dy = swap_tmp;
                swap_tmp = tex->exts;
                tex->exts = tex->mipmaps;
                tex->mipmaps = swap_tmp;
                // TODO: Not sure if we need to apply the same to hflags...
                tex->lflags = ((tex->lflags & 0xf0f0f0f0) >> 4) | ((tex->lflags & 0x0f0f0f0f) << 4);
            }
            uint64_t flags = (uint64_t)tex->hflags << 32 | tex->lflags;
            pos += sizeof(g1t_tex_header);
            uint32_t width = 1 << tex->dx;
            uint32_t height = 1 << tex->dy;
            uint32_t extra_size = (flags & G1T_FLAG_LOCAL_XDATA) ? getp32(&buf[pos]) : 0;
            // Non power-of-two width and height may be provided in the extra data
            if (extra_size >= 0x14) {
                if (width == 1)
                    width = getp32(&buf[pos + 0x0c]);
                if (height == 1)
                    height = getp32(&buf[pos + 0x10]);
            }

            JSON_Value* json_texture = json_value_init_object();
            snprintf(path, sizeof(path), "%03d.dds", i);
            json_object_set_string(json_object(json_texture), "name", path);
            json_object_set_number(json_object(json_texture), "type", tex->type);
            json_object_set_number(json_object(json_texture), "mipmaps", tex->mipmaps);
            json_object_set_number(json_object(json_texture), "flags", (double)flags);
            if (tex->exts != 0)
                json_object_set_number(json_object(json_texture), "exts", tex->exts);
            uint32_t texture_format = default_texture_format;
            bool swizzled = false;
            switch (tex->type) {
            case 0x00: break;
            case 0x01: break;
            case 0x02: break;
//            case 0x03: bpp = 64; break;
//            case 0x04: bpp = 128; break;
            case 0x06: texture_format = DDS_FORMAT_DXT1; break;
//            case 0x07: texture_format = DDS_FORMAT_DXT3; break;
            case 0x08: texture_format = DDS_FORMAT_DXT5; break;
            case 0x09: swizzled = true; break;
//            case 0x0A: swizzled = true; break;
            case 0x10: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;
            case 0x12: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;
            case 0x21: break;
            case 0x3C: texture_format = DDS_FORMAT_DXT1; break;
            case 0x3D: texture_format = DDS_FORMAT_DXT1; break;
            case 0x45: texture_format = DDS_FORMAT_BGR; swizzled = true; break;
            case 0x59: texture_format = DDS_FORMAT_DXT1; break;
            case 0x5B: texture_format = DDS_FORMAT_DXT5; break;
            case 0x5C: texture_format = DDS_FORMAT_BC4; break;
//            case 0x5D: texture_format = DDS_FORMAT_ATI1; break;
//            case 0x5E: texture_format = DDS_FORMAT_ATI2; break;
            case 0x5F: texture_format = DDS_FORMAT_BC7; break;
            case 0x60: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;
            case 0x62: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;
//            case 0x63: texture_format = DDS_FORMAT_BC4; swizzled = true; break;
//            case 0x64: texture_format = DDS_FORMAT_BC5; swizzled = true; break;
//            case 0x65: texture_format = DDS_FORMAT_BC6; swizzled = true; break;
//            case 0x66: texture_format = DDS_FORMAT_BC7; swizzled = true; break;
//            case 0x72: texture_format = DDS_FORMAT_????; break;
            default:
                fprintf(stderr, "ERROR: Unsupported texture type (0x%02X)\n", tex->type);
                fprintf(stderr, "Please visit: https://github.com/VitaSmith/gust_tools/issues/51\n");
                r = -1;
                continue;
            }
            uint32_t expected_texture_size = 0;
            for (int j = 0; j < tex->mipmaps; j++)
                expected_texture_size += MIPMAP_SIZE(texture_format, j, width, height);
            uint32_t texture_size = ((i + 1 == hdr->nb_textures) ?
                g1t_size - hdr->header_size : getv32(x_offset_table[i + 1])) - getv32(x_offset_table[i]);
            texture_size -= (uint32_t)sizeof(g1t_tex_header);
            if (flags & G1T_FLAG_LOCAL_XDATA) {
                assert(pos + extra_size < g1t_size);
                if ((extra_size < 8) || (extra_size % 4 != 0)) {
                    fprintf(stderr, "ERROR: Can't handle local extra_data of size 0x%08x\n", extra_size);
                    r = -1;
                    continue;
                } else if (!list_only) {
                    JSON_Value* json_extra_array_val = json_value_init_array();
                    JSON_Array* json_extra_array_obj = json_array(json_extra_array_val);
                    for (uint32_t j = 4; j < extra_size; j += 4)
                        json_array_append_number(json_extra_array_obj, getp32(&buf[pos + j]));
                    json_object_set_value(json_object(json_texture), "extra_data", json_extra_array_val);
                }
                pos += extra_size;
                texture_size -= extra_size;
            }
            if (texture_size < expected_texture_size) {
                fprintf(stderr, "ERROR: Actual texture size is smaller than expected size\n");
                r = -1;
                continue;
            } else if (texture_size > expected_texture_size) {
                if (texture_size % expected_texture_size != 0)
                    fprintf(stderr, "WARNING: Actual texture size is 0x%x bytes larger than expected size 0x%x\n",
                        texture_size - expected_texture_size, expected_texture_size);
                z_stack = texture_size / expected_texture_size;
                json_object_set_number(json_object(json_texture), "z_stack", z_stack);
                expected_texture_size = texture_size;
            }

            snprintf(path, sizeof(path), "%s%s%c%03d.dds", dir, _basename(argv[argc - 1]), PATH_SEP, i);
            char dims[16];
            snprintf(dims, sizeof(dims), "%dx%d", width, height);
            printf("0x%02x 0x%08x 0x%08x %s %-10s %-7d %-6d\n", tex->type,
                hdr->header_size + hdr->extra_size + getv32(x_offset_table[i]),
                texture_size, &path[strlen(dir)], dims, tex->mipmaps, z_stack);
            if (list_only) {
                json_value_free(json_texture);
                continue;
            }
            FILE* dst = fopen_utf8(path, "wb");
            if (dst == NULL) {
                fprintf(stderr, "ERROR: Can't create file '%s'\n", path);
                r = -1;
                continue;
            }
            uint32_t dds_magic = DDS_MAGIC;
            if (fwrite(&dds_magic, sizeof(dds_magic), 1, dst) != 1) {
                fprintf(stderr, "ERROR: Can't write magic\n");
                fclose(dst);
                r = -1;
                continue;
            }
            if (write_dds_header(dst, texture_format, width, height * z_stack,
                                 tex->mipmaps, flags) != 1) {
                fprintf(stderr, "ERROR: Can't write DDS header\n");
                fclose(dst);
                r = -1;
                continue;
            }

            // Non ARGB textures require conversion to be applied, since
            // tools like Visual Studio or PhotoShop can't be bothered
            // to honour the pixel format from the DDS header and instead
            // insist on using ARGB always...
            switch (texture_format) {
            case DDS_FORMAT_RGBA:
                rgba_convert(texture_format, "RGBA", "ARGB", &buf[pos], expected_texture_size);
                break;
            case DDS_FORMAT_ABGR:
                rgba_convert(texture_format, "ABGR", "ARGB", &buf[pos], expected_texture_size);
                break;
            case DDS_FORMAT_GRAB:
                rgba_convert(texture_format, "GRAB", "ARGB", &buf[pos], expected_texture_size);
                break;
            default:
                break;
            }
            if (swizzled) {
                int16_t mo = 0;     // Morton order
                uint32_t wf = 1;    // Width factor
                switch (hdr->platform) {
                case SONY_PS4:
                case NINTENDO_3DS:
                    mo = -3;
                    wf = 2;
                    break;
                case NINTENDO_WIIU:
                    wf = 2;
                    break;
                    // Fall through
                default:
                    mo = -1 * (int16_t)log2(min(width / dds_bwh(texture_format) / wf,
                        height / dds_bwh(texture_format)));
                    break;
                }
                uint32_t offset = 0;
                assert(mo != 0);
                for (int j = 1; j <= tex->mipmaps && mo != 0; j++) {
                    uint32_t mipmap_size = MIPMAP_SIZE(texture_format, j - 1, width, height);
                    mortonize(texture_format, mo, width / (1 << (j - 1)), height / (1 << (j - 1)),
                        &buf[pos + offset], mipmap_size, wf);
                    offset += mipmap_size;
                    mo += (mo > 0) ? -1 : +1;
                }
            }
            if (flip_image || ((hdr->platform == NINTENDO_3DS) && (tex->type == 0x09 || tex->type == 0x45)))
                flip(dds_bpp(texture_format), &buf[pos], expected_texture_size, width);
            if (fwrite(&buf[pos], expected_texture_size, 1, dst) != 1) {
                fprintf(stderr, "ERROR: Can't write DDS data\n");
                fclose(dst);
                continue;
            }
            fclose(dst);
            json_array_append_value(json_array(json_textures_array), json_texture);
        }

        json_object_set_value(json_object(json), "extra_flags", json_extra_flags_array);
        if (hdr->extra_size)
            json_object_set_value(json_object(json), "extra_data", json_global_extra_data_array);
        else
            json_value_free(json_global_extra_data_array);
        json_object_set_value(json_object(json), "textures", json_textures_array);
        snprintf(path, sizeof(path), "%s%cg1t.json", argv[argc - 1], PATH_SEP);
        if (!list_only)
            json_serialize_to_file_pretty(json, path);

        r = 0;
    }

out:
    json_value_free(json);
    free(buf);
    free(dir);
    free(offset_table);
    if (file != NULL)
        fclose(file);

    if (r != 0 && !no_prompt) {
        fflush(stdin);
        printf("\nPress any key to continue...");
        (void)getchar();
    }

    return r;
}

CALL_MAIN
