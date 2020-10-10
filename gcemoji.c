#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define BANK_LEN       0x2000
#define BANK_LOAD_ADDR 0x6000

#define IMAGE_BANK_SIZE 256
#define IMAGE_BANK_LEN ((IMAGE_BANK_SIZE * IMAGE_BANK_SIZE) / 4)

#define ICON_SIZE 64
#define ICON_LEN (ICON_SIZE * ICON_SIZE)

#define DISCORD_EMOJI_SIZE 128
#define DISCORD_EMOJI_LEN (DISCORD_EMOJI_SIZE * DISCORD_EMOJI_SIZE)

typedef struct
{
    uint8_t  size;
    uint8_t  entry_bank;
    uint16_t entry_address;
    uint8_t  flags;
    char     system[9];
    uint8_t  icon_bank;
    uint8_t  icon_x;
    uint8_t  icon_y;
    char     title[9];
    uint8_t  game_id[2];
    uint8_t  security_code;
    uint8_t  pad[3];
} rom_header_t;

const uint32_t gc_palette[4] =
{ 0xFFFFFFFF, 0xFFC0C0C0, 0xFF808080, 0xFF000000 };

// decode 2-bit game.com bitmap to 32-bit RGBA
void expand_gc(uint8_t *in, uint32_t *out, uint32_t w, uint32_t h)
{
    uint32_t  i, x, y;
    uint32_t *buf, *start;

    start = buf = malloc(w * h * sizeof(uint32_t));

    // decode 2-bit image
    for (i = 0; i < (w * h) / 4; i++)
    {
        *buf++ = gc_palette[(in[i] >> 6) & 3];
        *buf++ = gc_palette[(in[i] >> 4) & 3];
        *buf++ = gc_palette[(in[i] >> 2) & 3];
        *buf++ = gc_palette[(in[i] >> 0) & 3];
    }

    // rotate 270 degrees and flip horizontally
    for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
        out[y * w + (w - x - 1)] = start[y + ((w - x - 1) * h)];

    free(start);
}


// crop to 64x64 and upscale to 128x128 (discord emoji size) if specified
void crop_upscale_icon(uint32_t *in, uint32_t *out, uint8_t upscale, uint8_t in_bank, uint8_t bank_x, uint8_t bank_y)
{
    uint8_t  x, y;
    uint32_t where;
    
    for (y = 0; y < ICON_SIZE; y++)
    {
        for (x = 0; x < ICON_SIZE; x++)
        {
            where = in_bank ? (((bank_y + y) * IMAGE_BANK_SIZE) + bank_x + x) : ((y * ICON_SIZE) + x);
            
            if (upscale)
            {
                out[0] = out[1] = in[where];
                out += 2;
            }
            else
            {
                *out++ = in[where];
            }
        }
        
        if (upscale)
        {
            memcpy(out, out - DISCORD_EMOJI_SIZE, DISCORD_EMOJI_SIZE * sizeof(uint32_t));
            out += DISCORD_EMOJI_SIZE;
        }
    }
}

uint32_t decompress(uint8_t *in, uint8_t *out, uint32_t in_len)
{
    uint8_t *in_end, *out_start;
    uint16_t copy;
    
    in_end    = in + in_len;
    out_start = out;
    
    while (in < in_end)
    {
        copy = 0;
        
        if (*in == 0xc0) // 16-bit RLE
        {
            copy = (in[2] << 8) | in[1];
            memset(out, in[3], copy);
            in += 4;
        }
        else if (*in > 0xc0) // 8-bit RLE
        {
            copy = *in & 0x3f;
            memset(out, in[1], copy);
            in += 2;
        }
        else // raw copy
        {
            *out++ = *in++;
        }
        
        out += copy;
    }
    
    return out - out_start;
}

int main(int argc, char **argv)
{
    FILE *rom;    
    rom_header_t hdr;
    uint32_t offset, upscale;
    
    char outfn[FILENAME_MAX];
    static uint32_t icon[DISCORD_EMOJI_SIZE * DISCORD_EMOJI_SIZE];    
    
    if (argc < 2)
    {
        printf("Usage: %s rom.bin [icon.png] [-u]\n", argv[0]);
        return 0;
    }
    
    if ((rom = fopen(argv[1], "rb")) == NULL)
    {
        perror("Error opening ROM");
        return 1;
    }
        
    // read ROM header and bank
    fread(&hdr, 1, sizeof(rom_header_t), rom);
    
    offset = 0;
    
    // if it's a bad rom dump, skip 0x40000 and read again
    if (hdr.size == 0 || hdr.size == 0xff)
    {
        fseek(rom, 0x40000, SEEK_SET);
        fread(&hdr, 1, sizeof(rom_header_t), rom);
        
        offset += 0x40000;
    }
    
    // make filename
    if (argc >= 3)
        strcpy(outfn, argv[2]);
    else
        sprintf(outfn, "%s-%.9s.png", argv[1], hdr.title);

    upscale = (argc >= 4 && argv[3][0] == '-' && argv[3][1] == 'u');
    
    // icon flag
    if (((hdr.flags >> 1) & 1) == 0)
    {
        fputs("Error: Game has no icon\n", stderr);
        fclose(rom);
        return 1;
    }
    
    // compressed flag
    if ((hdr.flags >> 3) & 1)
    {
        // lengths technically incorrect, but compressed size isn't stored so this is the best we can do
        static uint8_t  icon_buf[ICON_LEN];
        static uint8_t  icon_dec[ICON_LEN];
        static uint32_t icon_rgb[ICON_LEN];
        
        // calculate icon offset
        offset = ((hdr.icon_bank - 0x20) * BANK_LEN) + (((hdr.icon_x << 8) | hdr.icon_y) - BANK_LOAD_ADDR);
        fseek(rom, offset, SEEK_SET);
        fread(icon_buf, 1, ICON_LEN, rom);
        
        // decompress, decode and upscale
        decompress(icon_buf, icon_dec, ICON_LEN);
        expand_gc(icon_dec, icon_rgb, ICON_SIZE, ICON_SIZE);
        crop_upscale_icon(icon_rgb, icon, upscale, 0, 0, 0);
    }
    else
    {
        static uint8_t  bank_buf[IMAGE_BANK_LEN];
        static uint32_t bank_img[IMAGE_BANK_SIZE * IMAGE_BANK_SIZE];
        
        // calculate icon offset
        offset += (hdr.icon_bank - (hdr.entry_bank / 2)) * IMAGE_BANK_LEN;
        
        fseek(rom, offset, SEEK_SET);
        fread(bank_buf, 1, IMAGE_BANK_LEN, rom);
        
        // decode bank and get icon
        expand_gc(bank_buf, bank_img, IMAGE_BANK_SIZE, IMAGE_BANK_SIZE);
        crop_upscale_icon(bank_img, icon, upscale, 1, hdr.icon_x, hdr.icon_y);
    }
    
    fclose(rom);
    
    // write PNG
    if (upscale)
    {
        if (stbi_write_png(outfn, DISCORD_EMOJI_SIZE, DISCORD_EMOJI_SIZE, 4, icon, DISCORD_EMOJI_SIZE * 4) == 0)
        {
            fputs("Error writing PNG\n", stderr);
            return 1;
        }
    }
    else
    {
        if (stbi_write_png(outfn, ICON_SIZE, ICON_SIZE, 4, icon, ICON_SIZE * 4) == 0)
        {
            fputs("Error writing PNG\n", stderr);
            return 1;
        }
    }
    
    puts("Done!");
    
    return 0;
}
