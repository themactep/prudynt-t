
#include <cmath>
#include "OSD.hpp"
#include "Config.hpp"
#include <pthread.h>
#include "Logger.hpp"
#include "globals.hpp"
#include <unistd.h>
#include <vector>

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPEncoderCHNAttr IMPEncoderChnAttr
#define IMPEncoderCHNStat IMPEncoderChnStat
#endif

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define picWidth uWidth
#define picHeight uHeight
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <fstream>

#include "schrift.h"

int OSD::renderGlyph(const char *characters)
{

    while (*characters)
    {

        SFT_LMetrics lmetrics;
        SFT_GMetrics gmetrics;
        SFT_Glyph glyph;
        SFT_Image imageBuffer;

        if (sft_lmetrics(sft, &lmetrics) == 0 && sft_lookup(sft, *characters, &glyph) == 0)
        {
            if (sft_gmetrics(sft, glyph, &gmetrics) == 0)
            {
                imageBuffer.width = gmetrics.minWidth;
                imageBuffer.height = gmetrics.minHeight;
                imageBuffer.pixels = (uint8_t *)malloc(imageBuffer.width * imageBuffer.height);

                if (sft_render(sft, glyph, imageBuffer) == 0)
                {
                    Glyph g;
                    g.width = imageBuffer.width;
                    g.height = imageBuffer.height;
                    g.advance = gmetrics.advanceWidth;
                    g.xmin = gmetrics.leftSideBearing;
                    g.ymin = gmetrics.yOffset;
                    g.glyph = glyph;

                    // Store only alpha values, colors will be applied during drawing
                    g.bitmap.resize(g.width * g.height);
                    for (int y = 0; y < g.height; ++y)
                    {
                        for (int x = 0; x < g.width; ++x)
                        {
                            int pixelIndex = y * g.width + x;
                            uint8_t alpha = ((uint8_t *)imageBuffer.pixels)[pixelIndex];
                            g.bitmap[pixelIndex] = alpha;
                        }
                    }

                    glyphs[*characters] = g;
                }
                free(imageBuffer.pixels);
            }
        }
        ++characters;
    }

    return 0;
}

void setPixel(uint8_t *image, int x, int y, const uint8_t *color, int WIDTH, int HEIGHT)
{
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
    {
        int index = (y * WIDTH + x) * 4;
        image[index] = color[0];     // B
        image[index + 1] = color[1]; // G
        image[index + 2] = color[2]; // R
        image[index + 3] = color[3]; // A
    }
}

void OSD::drawOutline(uint8_t* image, const Glyph& g, int x, int y, int outlineSize, int WIDTH, int HEIGHT, const uint8_t* strokeColor) {
    for (int j = -outlineSize; j <= outlineSize; ++j) {
        for (int i = -outlineSize; i <= outlineSize; ++i) {
            if (i * i + j * j <= outlineSize * outlineSize) { // Use circular distance
                for (int h = 0; h < g.height; ++h) {
                    for (int w = 0; w < g.width; ++w) {
                        int srcIndex = h * g.width + w;
                        uint8_t glyphAlpha = g.bitmap[srcIndex];
                        if (glyphAlpha > 0) { // Check alpha value
                            // Combine glyph alpha with stroke color alpha
                            uint8_t combinedAlpha = (uint8_t)((glyphAlpha * strokeColor[3]) / 255);
                            uint8_t pixelColor[4] = {strokeColor[0], strokeColor[1], strokeColor[2], combinedAlpha};
                            setPixel(image, x + w + i, y + h + j, pixelColor, WIDTH, HEIGHT);
                        }
                    }
                }
            }
        }
    }
}
/*
void OSD::drawOutline(uint8_t *image, const Glyph &g, int x, int y, int outlineSize, int WIDTH, int HEIGHT)
{
    for (int j = -outlineSize; j <= outlineSize; ++j)
    {
        for (int i = -outlineSize; i <= outlineSize; ++i)
        {
            if (abs(i) + abs(j) <= outlineSize)
            { // Use Manhattan distance
                for (int h = 0; h < g.height; ++h)
                {
                    for (int w = 0; w < g.width; ++w)
                    {
                        int srcIndex = (h * g.width + w) * 4;
                        if (g.bitmap[srcIndex + 3] > 0)
                        { // Check alpha value
                            setPixel(image, x + w + i, y + h + j, BGRA_STROKE, WIDTH, HEIGHT);
                        }
                    }
                }
            }
        }
    }
}
*/
int OSD::drawText(uint8_t *image, const char *text, int WIDTH, int HEIGHT, int outlineSize, unsigned int font_color, unsigned int font_stroke_color)
{
    int penX = 1;
    int penY = 1;

    // Extract BGRA components from colors
    uint8_t textColor[4] = {
        (uint8_t)(font_color & 0xFF),         // B
        (uint8_t)((font_color >> 8) & 0xFF),  // G
        (uint8_t)((font_color >> 16) & 0xFF), // R
        (uint8_t)((font_color >> 24) & 0xFF)  // A
    };

    uint8_t strokeColor[4] = {
        (uint8_t)(font_stroke_color & 0xFF),         // B
        (uint8_t)((font_stroke_color >> 8) & 0xFF),  // G
        (uint8_t)((font_stroke_color >> 16) & 0xFF), // R
        (uint8_t)((font_stroke_color >> 24) & 0xFF)  // A
    };

    // Draw text and outline
    while (*text)
    {
        auto it = glyphs.find(*text);
        if (it != glyphs.end())
        {
            const Glyph &g = it->second;

            int x = penX + g.xmin + outlineSize;
            int y = penY + (sft->yScale + g.ymin);

            // Draw the outline (only if stroke size > 0)
            if (outlineSize > 0) {
                drawOutline(image, g, x, y, outlineSize, WIDTH, HEIGHT, strokeColor);
            }

            // Draw the actual text
            for (int j = 0; j < g.height; ++j)
            {
                for (int i = 0; i < g.width; ++i)
                {
                    int srcIndex = j * g.width + i;
                    uint8_t glyphAlpha = g.bitmap[srcIndex];
                    if (glyphAlpha > 0)
                    {
                        // Combine glyph alpha with text color alpha
                        uint8_t combinedAlpha = (uint8_t)((glyphAlpha * textColor[3]) / 255);
                        uint8_t pixelColor[4] = {textColor[0], textColor[1], textColor[2], combinedAlpha};
                        setPixel(image, x + i, y + j, pixelColor, WIDTH, HEIGHT);
                    }
                }
            }

            penX += g.advance + (outlineSize * 2);
        }
        ++text;
    }

    return 0;
}

int OSD::calculateTextSize(const char *text, uint16_t &width, uint16_t &height, int outlineSize)
{
    width = 0;
    height = 0;

    while (*text)
    {
        auto it = glyphs.find(*text);
        if (it != glyphs.end())
        {
            const Glyph &g = it->second;

            width += g.advance + (outlineSize * 2);
            if (g.height > height)
            {
                height = g.height;
            }
        }

        ++text;
    }

    height += sft->yScale;
    width += 1 + outlineSize;

    return 0;
}

int OSD::libschrift_init()
{
    LOG_DEBUG("OSD::libschrift_init()");

    std::ifstream fontFile(osd.font_path, std::ios::binary | std::ios::ate);
    if (!fontFile.is_open())
    {
        LOG_DEBUG("Unable to open font file.");
        return -1;
    }

    size_t fileSize = fontFile.tellg();
    std::vector<uint8_t> fontData;
    fontFile.seekg(0, std::ios::beg);
    fontData.resize(fileSize);
    fontFile.read(reinterpret_cast<char *>(fontData.data()), fileSize);
    fontFile.close();

    sft = new SFT();
    sft->flags = SFT_DOWNWARD_Y;
    sft->xScale = osd.font_size;
    sft->yScale = osd.font_size;
    int yoff = (int)round((float)sft->yScale * 0.1f);
    if (yoff < 1) yoff = 1;
    sft->yOffset = yoff;
    sft->font = sft_loadmem(fontData.data(), fontData.size());
    if (!sft->font)
    {
        LOG_DEBUG("Unable to load font data.");
        return -1;
    }

    renderGlyph("01234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!§$%&/()=?,.-_:;#'+*~}{} ");

    fontData.clear();
    return 0;
}

void OSD::set_text(OSDItem *osdItem, IMPOSDRgnAttr *irgnAttr, const char *text, const char *position, int angle, unsigned int font_color, unsigned int font_stroke_color)
{
    // parse position string "x,y"
    int posX = 0, posY = 0;
    if (position && *position) {
        const char *comma = strchr(position, ',');
        if (comma) {
            char xb[16] = {0};
            char yb[16] = {0};
            size_t xl = (size_t)(comma - position);
            size_t yl = strlen(comma + 1);
            if (xl > 0 && xl < sizeof(xb)) { memcpy(xb, position, xl); xb[xl] = '\0'; posX = atoi(xb); }
            else { LOG_ERROR("Invalid X in position: " << (position ? position : "")); }
            if (yl > 0 && yl < sizeof(yb)) { memcpy(yb, comma + 1, yl); yb[yl] = '\0'; posY = atoi(yb); }
            else { LOG_ERROR("Invalid Y in position: " << (position ? position : "")); }
        } else {
            LOG_ERROR("Invalid position format (expected x,y): " << position);
        }
    }

    // size and stroke
    uint8_t stroke_width = osd.font_stroke_size;
    uint16_t item_width = 0;
    uint16_t item_height = 0;

    calculateTextSize(text, item_width, item_height, stroke_width);

    if (item_width % 2 != 0)
        ++item_width;

    int item_size = item_width * item_height * 4;

    free(osdItem->data);
    osdItem->data = (uint8_t *)malloc(item_size);
    memset(osdItem->data, 0, item_size);

    drawText(osdItem->data, text, item_width, item_height, stroke_width, font_color, font_stroke_color);

    if (angle)
    {
        rotateBGRAImage(osdItem->data, item_width, item_height, angle, true);
    }

    if (item_width != osdItem->width || item_height != osdItem->height)
    {
        if (irgnAttr == nullptr)
        {
            IMP_OSD_GetRgnAttr(osdItem->imp_rgn, &osdItem->rgnAttr);
        }

        set_pos(&osdItem->rgnAttr, posX, posY, item_width, item_height, stream_width, stream_height);

        osdItem->rgnAttr.data.picData.pData = osdItem->data;
        osdItem->rgnAttrData = &osdItem->rgnAttr.data;

        osdItem->width = item_width;
        osdItem->height = item_height;

        IMP_OSD_SetRgnAttr(osdItem->imp_rgn, &osdItem->rgnAttr);
    }
    else
    {
        osdItem->rgnAttrData->picData.pData = osdItem->data;
        IMP_OSD_UpdateRgnAttrData(osdItem->imp_rgn, osdItem->rgnAttrData);
    }

    return;
}

unsigned long getSystemUptime()
{
    struct sysinfo info;
    if (sysinfo(&info) != 0)
    {
        return 0;
    }
    return info.uptime;
}

int getIp(char *addressBuffer)
{
    struct ifaddrs *ifAddrStruct = nullptr;
    struct ifaddrs *ifa = nullptr;
    void *tmpAddrPtr = nullptr;

    getifaddrs(&ifAddrStruct);

    for (ifa = ifAddrStruct; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr)
        {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_INET)
        { // check it is IP4
            tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
        }
    }
    if (ifAddrStruct != nullptr)
        freeifaddrs(ifAddrStruct);
    return 0;
}

std::string OSD::getConfigPath(const char *itemName)
{
    return std::string(parent) + ".osd." + itemName;
}

int autoFontSize(int pWidth)
{
    double m = 0.0046875;
    double b = 9.0;
    return static_cast<int>(m * pWidth + b + 0.5);
}

void replace(std::string& str, const std::string& oldToken, const std::string& newToken) {
    size_t pos = 0;
    while ((pos = str.find(oldToken, pos)) != std::string::npos) {
        str.replace(pos, oldToken.length(), newToken);
        pos += newToken.length();
    }
}

void OSD::rotateBGRAImage(uint8_t *&inputImage, uint16_t &width, uint16_t &height, int angle, bool del = true)
{
    double angleRad = angle * (M_PI / 180.0);

    int originalCorners[4][2] = {
        {0, 0},
        {width, 0},
        {0, height},
        {width, height}};

    int minX = INT_MAX;
    int maxX = INT_MIN;
    int minY = INT_MAX;
    int maxY = INT_MIN;

    for (auto &originalCorner : originalCorners)
    {
        int x = originalCorner[0];
        int y = originalCorner[1];

        int newX = static_cast<int>(x * cos(angleRad) - y * sin(angleRad));
        int newY = static_cast<int>(x * sin(angleRad) + y * cos(angleRad));

        if (newX < minX)
            minX = newX;
        if (newX > maxX)
            maxX = newX;
        if (newY < minY)
            minY = newY;
        if (newY > maxY)
            maxY = newY;
    }

    int newWidth = maxX - minX + 1;
    int newHeight = maxY - minY + 1;

    int centerX = width / 2;
    int centerY = height / 2;

    int newCenterX = newWidth / 2;
    int newCenterY = newHeight / 2;

    auto *rotatedImage = new uint8_t[newWidth * newHeight * 4]();

    for (int y = 0; y < newHeight; ++y)
    {
        for (int x = 0; x < newWidth; ++x)
        {
            int newX = x - newCenterX;
            int newY = y - newCenterY;

            int origX = static_cast<int>(newX * cos(angleRad) + newY * sin(angleRad)) + centerX;
            int origY = static_cast<int>(-newX * sin(angleRad) + newY * cos(angleRad)) + centerY;

            if (origX >= 0 && origX < width && origY >= 0 && origY < height)
            {
                for (int c = 0; c < 4; ++c)
                {
                    rotatedImage[(y * newWidth + x) * 4 + c] = inputImage[(origY * width + origX) * 4 + c];
                }
            }
        }
    }

    if (del)
        delete[] inputImage;
    inputImage = rotatedImage;
    width = newWidth;
    height = newHeight;
}

uint16_t OSD::get_abs_pos(const uint16_t max, const uint16_t size, const int pos)
{
    if (pos == 0)
    {
        return max / 2 - size / 2;
    }
    if (pos < 0)
    {
        return max - size - 1 + pos;
    }
    return pos;
}

void OSD::set_pos(IMPOSDRgnAttr *rgnAttr, int x, int y, uint16_t width, uint16_t height, const uint16_t max_width, const uint16_t max_height)
{
    if (width == 0 || height == 0)
    {
        width = rgnAttr->rect.p1.x - rgnAttr->rect.p0.x + 1;
        height = rgnAttr->rect.p1.y - rgnAttr->rect.p0.y + 1;
    }

    if (x > max_width - width)
        x = max_width - width;

    if (y > max_height - height)
        y = max_height - height;

    rgnAttr->rect.p0.x = get_abs_pos(max_width, width, x);
    rgnAttr->rect.p0.y = get_abs_pos(max_height, height, y);
    rgnAttr->rect.p1.x = rgnAttr->rect.p0.x + width - 1;
    rgnAttr->rect.p1.y = rgnAttr->rect.p0.y + height - 1;
}

unsigned char *loadBGRAImage(const char *filepath, size_t &length)
{
    FILE *file = fopen(filepath, "rb");
    if (!file)
    {
        printf("Failed to open the OSD logo file.\n");
        return nullptr;
    }

    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);

    unsigned char *data = (unsigned char *)malloc(length);
    if (!data)
    {
        printf("Failed to allocate memory for the image.\n");
        fclose(file);
        return nullptr;
    }

    if (fread(data, 1, length, file) != length)
    {
        printf("Failed to read OSD logo data.\n");
        free(data);
        fclose(file);
        return nullptr;
    }

    fclose(file);
    return data;
}

OSD *OSD::createNew(
    _osd &osd,
    int osdGrp,
    int encChn,
    const char *parent)
{
    return new OSD(osd, osdGrp, encChn, parent);
}

void OSD::init()
{
    int ret = 0;
    LOG_DEBUG("OSD init for begin");

    ret = IMP_OSD_SetPoolSize(cfg->general.osd_pool_size * 1024);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetPoolSize(" << (cfg->general.osd_pool_size * 1024) << ")");

    // cfg = _cfg;
    last_updated_second = -1;

    ret = IMP_Encoder_GetChnAttr(osdGrp, &channelAttributes);
    if (ret < 0)
    {
        LOG_DEBUG("IMP_Encoder_GetChnAttr() == " << ret);
        // return true;
    }

    stream_width = channelAttributes.encAttr.picWidth;
    stream_height = channelAttributes.encAttr.picHeight;

    // Calculate realistic OSD pool size based on typical usage
    // Estimate: ~10% of screen area for OSD elements (text, logos, etc.)
    // Formula: (width * height * 4 bytes * 0.1) / 1024 + safety margin
    int estimated_usage = (stream_width * stream_height * 4 * 0.1) / 1024; // 10% screen coverage
    int safety_margin = 256; // 256KB safety margin
    int recommended_pool_size = estimated_usage + safety_margin;
    int configured_pool_size = cfg->general.osd_pool_size;

    // Only warn if configured size is significantly smaller than recommended
    if (configured_pool_size < recommended_pool_size) {
        LOG_WARN("OSD pool size (" << configured_pool_size << "KB) may be insufficient for "
                 << stream_width << "x" << stream_height << " resolution with large fonts/logos. "
                 << "Consider increasing to " << recommended_pool_size << "KB for optimal performance");
    } else {
        LOG_DEBUG("OSD pool size (" << configured_pool_size << "KB) is adequate for "
                  << stream_width << "x" << stream_height << " resolution");
    }

    // picWidth, picHeight cpp macro !!
    LOG_DEBUG("IMP_Encoder_GetChnAttr read. Stream resolution: " << stream_width
                                                                 << "x" << stream_height);

    ret = IMP_OSD_CreateGroup(osdGrp);

    int fontSize = autoFontSize(channelAttributes.encAttr.picWidth);
    // int autoOffset = round((float)(channelAttributes.encAttr.picWidth * 0.004)); // Currently unused

    if (osd.font_size == OSD_AUTO_VALUE)
    {
        // use cfg->set to set noSave, so auto values will not written to config
        cfg->set<int>(getConfigPath("font_size"), fontSize, true);
    }

    if (libschrift_init() != 0)
    {
        LOG_DEBUG("libschrift init failed.");
    }

    if (osd.time_enabled)
    {
        /* OSD Time */

        osdTime.data = nullptr;
        osdTime.imp_rgn = IMP_OSD_CreateRgn(nullptr);
        IMP_OSD_RegisterRgn(osdTime.imp_rgn, osdGrp, nullptr);
        osd.regions.time = osdTime.imp_rgn;

        memset(&osdTime.rgnAttr, 0, sizeof(IMPOSDRgnAttr));
        osdTime.rgnAttr.type = OSD_REG_PIC;
        osdTime.rgnAttr.fmt = PIX_FMT_BGRA;
        set_text(&osdTime, &osdTime.rgnAttr, osd.time_format,
                 osd.time_position, osd.time_rotation,
                 osd.time_font_color, osd.time_font_stroke_color);
        IMP_OSD_SetRgnAttr(osdTime.imp_rgn, &osdTime.rgnAttr);

        IMPOSDGrpRgnAttr grpRgnAttr;
        memset(&grpRgnAttr, 0, sizeof(IMPOSDGrpRgnAttr));
        grpRgnAttr.show = 1;
        grpRgnAttr.layer = 1;
        grpRgnAttr.gAlphaEn = 1;  // Enable alpha blending for per-pixel transparency
        grpRgnAttr.fgAlhpa = 255; // Full foreground alpha to allow per-pixel alpha control
        grpRgnAttr.bgAlhpa = 0;   // Transparent background
        IMP_OSD_SetGrpRgnAttr(osdTime.imp_rgn, osdGrp, &grpRgnAttr);
    }

    if (osd.usertext_enabled)
    {
        getIp(ip);
        gethostname(hostname, 64);

        /* OSD Usertext */

        osdUser.data = nullptr;
        osdUser.imp_rgn = IMP_OSD_CreateRgn(nullptr);
        IMP_OSD_RegisterRgn(osdUser.imp_rgn, osdGrp, nullptr);
        osd.regions.user = osdUser.imp_rgn;

        memset(&osdUser.rgnAttr, 0, sizeof(IMPOSDRgnAttr));
        osdUser.rgnAttr.type = OSD_REG_PIC;
        osdUser.rgnAttr.fmt = PIX_FMT_BGRA;
        set_text(&osdUser, &osdUser.rgnAttr, osd.usertext_format,
                 osd.usertext_position, osd.usertext_rotation,
                 osd.usertext_font_color, osd.usertext_font_stroke_color);
        IMP_OSD_SetRgnAttr(osdUser.imp_rgn, &osdUser.rgnAttr);

        IMPOSDGrpRgnAttr grpRgnAttr;
        memset(&grpRgnAttr, 0, sizeof(IMPOSDGrpRgnAttr));
        grpRgnAttr.show = 1;
        grpRgnAttr.layer = 2;
        grpRgnAttr.gAlphaEn = 1;  // Enable alpha blending for per-pixel transparency
        grpRgnAttr.fgAlhpa = 255; // Full foreground alpha to allow per-pixel alpha control
        grpRgnAttr.bgAlhpa = 0;   // Transparent background
        IMP_OSD_SetGrpRgnAttr(osdUser.imp_rgn, osdGrp, &grpRgnAttr);
    }

    if (osd.uptime_enabled)
    {
        /* OSD Uptime */

        osdUptm.data = nullptr;
        osdUptm.imp_rgn = IMP_OSD_CreateRgn(nullptr);
        IMP_OSD_RegisterRgn(osdUptm.imp_rgn, osdGrp, nullptr);
        osd.regions.uptime = osdUptm.imp_rgn;

        memset(&osdUptm.rgnAttr, 0, sizeof(IMPOSDRgnAttr));
        osdUptm.rgnAttr.type = OSD_REG_PIC;
        osdUptm.rgnAttr.fmt = PIX_FMT_BGRA;
        set_text(&osdUptm, &osdUptm.rgnAttr, osd.uptime_format,
                 osd.uptime_position, osd.uptime_rotation,
                 osd.uptime_font_color, osd.uptime_font_stroke_color);
        IMP_OSD_SetRgnAttr(osdUptm.imp_rgn, &osdUptm.rgnAttr);

        IMPOSDGrpRgnAttr grpRgnAttr;
        memset(&grpRgnAttr, 0, sizeof(IMPOSDGrpRgnAttr));
        grpRgnAttr.show = 1;
        grpRgnAttr.layer = 3;
        grpRgnAttr.gAlphaEn = 1;  // Enable alpha blending for per-pixel transparency
        grpRgnAttr.fgAlhpa = 255; // Full foreground alpha to allow per-pixel alpha control
        grpRgnAttr.bgAlhpa = 0;   // Transparent background
        IMP_OSD_SetGrpRgnAttr(osdUptm.imp_rgn, osdGrp, &grpRgnAttr);
    }

    if (osd.logo_enabled)
    {
        /* OSD Logo */

        size_t imageSize;
        auto imageData = loadBGRAImage(osd.logo_path, imageSize);

        osdLogo.data = nullptr;
        osdLogo.imp_rgn = IMP_OSD_CreateRgn(nullptr);
        IMP_OSD_RegisterRgn(osdLogo.imp_rgn, osdGrp, nullptr);
        osd.regions.logo = osdLogo.imp_rgn;

        memset(&osdLogo.rgnAttr, 0, sizeof(IMPOSDRgnAttr));

        // Verify OSD logo size vs dimensions
        if ((osd.logo_width * osd.logo_height * 4) == (int)imageSize)
        {
            osdLogo.rgnAttr.type = OSD_REG_PIC;
            osdLogo.rgnAttr.fmt = PIX_FMT_BGRA;
            osdLogo.rgnAttr.data.picData.pData = imageData;

            // Logo rotation
            uint16_t logo_width = osd.logo_width;
            uint16_t logo_height = osd.logo_height;
            if (osd.logo_rotation)
            {
                uint8_t *imageData = static_cast<uint8_t *>(osdLogo.rgnAttr.data.picData.pData);
                rotateBGRAImage(imageData, logo_width,
                                logo_height, osd.logo_rotation, false);
                osdLogo.rgnAttr.data.picData.pData = imageData;
            }

            // Parse logo_position string "x,y"
            int logoPosX = 0, logoPosY = 0;
            if (osd.logo_position && *osd.logo_position) {
                const char *comma = strchr(osd.logo_position, ',');
                if (comma) {
                    char xb[16] = {0};
                    char yb[16] = {0};
                    size_t xl = (size_t)(comma - osd.logo_position);
                    size_t yl = strlen(comma + 1);
                    if (xl > 0 && xl < sizeof(xb)) { memcpy(xb, osd.logo_position, xl); xb[xl] = '\0'; logoPosX = atoi(xb); }
                    else { LOG_ERROR("Invalid logo_position X: " << (osd.logo_position ? osd.logo_position : "")); }
                    if (yl > 0 && yl < sizeof(yb)) { memcpy(yb, comma + 1, yl); yb[yl] = '\0'; logoPosY = atoi(yb); }
                    else { LOG_ERROR("Invalid logo_position Y: " << (osd.logo_position ? osd.logo_position : "")); }
                } else {
                    LOG_ERROR("Invalid logo_position format (expected x,y): " << osd.logo_position);
                }
            }

            set_pos(&osdLogo.rgnAttr, logoPosX,
                    logoPosY, logo_width, logo_height, stream_width, stream_height);
        }
        else
        {

            LOG_ERROR("Invalid OSD logo dimensions. Imagesize=" << imageSize << ", " << osd.logo_width
                                                                << "*" << osd.logo_height << "*4=" << (osd.logo_width * osd.logo_height * 4));
        }
            // Parse logo_position string "x,y" - TODO: implement position parsing
            // int logoPosX = 0, logoPosY = 0;
            if (osd.logo_position && *osd.logo_position) {
                // TODO: Parse position string and set logo position
            }

        IMP_OSD_SetRgnAttr(osdLogo.imp_rgn, &osdLogo.rgnAttr);

        IMPOSDGrpRgnAttr grpRgnAttr;
        memset(&grpRgnAttr, 0, sizeof(IMPOSDGrpRgnAttr));
        grpRgnAttr.show = 1;
        grpRgnAttr.layer = 4;
        grpRgnAttr.gAlphaEn = 1;
        grpRgnAttr.fgAlhpa = osd.logo_transparency;
        IMP_OSD_SetGrpRgnAttr(osdLogo.imp_rgn, osdGrp, &grpRgnAttr);
    }

    if(osd.start_delay)
        startup_delay = (int)(osd.start_delay*1000)/THREAD_SLEEP;

    //start();
}

int OSD::start()
{
    int ret;

    ret = IMP_OSD_Start(osdGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_Start(" << osdGrp << ")");

    ret = IMP_OSD_SetPoolSize(cfg->general.osd_pool_size * 1024);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetPoolSize(" << (cfg->general.osd_pool_size * 1024) << ")");

    is_started = true;

    return ret;
}

int OSD::exit()
{
    int ret;

    ret = IMP_OSD_Stop(osdGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_Stop(" << osdGrp << ")");

    ret = IMP_OSD_ShowRgn(osdTime.imp_rgn, osdGrp, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_ShowRgn(osdTime.imp_rgn, " << osdGrp << ", 0)");

    ret = IMP_OSD_ShowRgn(osdUser.imp_rgn, osdGrp, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_ShowRgn(osdUser.imp_rgn, " << osdGrp << ", 0)");

    ret = IMP_OSD_ShowRgn(osdUptm.imp_rgn, osdGrp, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_ShowRgn(osdUptm.imp_rgn, " << osdGrp << ", 0)");

    ret = IMP_OSD_ShowRgn(osdLogo.imp_rgn, osdGrp, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_ShowRgn(osdLogo.imp_rgn, " << osdGrp << ", 0)");

    ret = IMP_OSD_UnRegisterRgn(osdTime.imp_rgn, osdGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_UnRegisterRgn(osdTime.imp_rgn, " << osdGrp << ")");

    ret = IMP_OSD_UnRegisterRgn(osdUser.imp_rgn, osdGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_UnRegisterRgn(osdUser.imp_rgn, " << osdGrp << ")");

    ret = IMP_OSD_UnRegisterRgn(osdUptm.imp_rgn, osdGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_UnRegisterRgn(osdUptm.imp_rgn, " << osdGrp << ")");

    ret = IMP_OSD_UnRegisterRgn(osdLogo.imp_rgn, osdGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_UnRegisterRgn(osdUptm.imp_rgn, " << osdGrp << ")");

    IMP_OSD_DestroyRgn(osdTime.imp_rgn);
    IMP_OSD_DestroyRgn(osdUser.imp_rgn);
    IMP_OSD_DestroyRgn(osdUptm.imp_rgn);
    IMP_OSD_DestroyRgn(osdLogo.imp_rgn);

    ret = IMP_OSD_DestroyGroup(osdGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_DestroyGroup(" << osdGrp << ")");

    // cleanup osd image data
    free(osdTime.data);
    free(osdUser.data);
    free(osdUptm.data);
    free(osdLogo.data);

    sft_freefont(sft->font);
    return 0;
}

void OSD::updateDisplayEverySecond()
{
    gettimeofday(&tm, NULL);

    current = time(nullptr);
    ltime = localtime(&current);

    // Check if we have moved to a new second
    if (ltime->tm_sec != last_updated_second)
    {
        flag |= 7;
        // Update the last second tracker
        last_updated_second = ltime->tm_sec;
    }
    else
    {
        // The flag ensures that only 1 OSD object is updated
        // on calling OSD::updateDisplayEverySecond()
        // this should relieve the system
        if (flag != 0)
        {
            // Format and update system time
            if ((flag & 1) && osd.time_enabled)
            {
                strftime(timeFormatted, sizeof(timeFormatted), osd.time_format, ltime);

                set_text(&osdTime, nullptr, timeFormatted,
                         osd.time_position, osd.time_rotation,
                         osd.time_font_color, osd.time_font_stroke_color);

                flag ^= 1;
                return;
            }

            // Format and update user text
            if ((flag & 2) && osd.usertext_enabled)
            {
                std::string usertext = osd.usertext_format;

                if (strstr(osd.usertext_format, "%hostname") != nullptr)
                {
                    replace(usertext, "%hostname", hostname);
                }

                if (strstr(osd.usertext_format, "%ipaddress") != nullptr)
                {
                    replace(usertext, "%ipaddress", ip);
                }

                if (strstr(osd.usertext_format, "%fps") != nullptr)
                {
                    char fps[4];
                    snprintf(fps, 4, "%3d", osd.stats.fps);
                    replace(usertext, "%fps", fps);
                }

                if (strstr(osd.usertext_format, "%bps") != nullptr)
                {
                    char bps[8];
                    snprintf(bps, 8, "%5d", osd.stats.bps);
                    replace(usertext, "%bps", bps);
                }

                set_text(&osdUser, nullptr, usertext.c_str(),
                         osd.usertext_position, osd.usertext_rotation,
                         osd.usertext_font_color, osd.usertext_font_stroke_color);

                usertext.clear();

                flag ^= 2;
                return;
            }

            // Format and update uptime
            if ((flag & 4) && osd.uptime_enabled)
            {
                unsigned long currentUptime = getSystemUptime();
                unsigned long days = currentUptime / 86400;
                unsigned long hours = (currentUptime % 86400) / 3600;
                unsigned long minutes = (currentUptime % 3600) / 60;
                //unsigned long seconds = currentUptime % 60;

                snprintf(uptimeFormatted, sizeof(uptimeFormatted), osd.uptime_format, days, hours, minutes);

                set_text(&osdUptm, nullptr, uptimeFormatted,
                         osd.uptime_position, osd.uptime_rotation,
                         osd.uptime_font_color, osd.uptime_font_stroke_color);

                flag ^= 4;
                return;
            }
        }
    }
}

void *OSD::thread_entry(void *arg) {
    LOG_DEBUG("start osd update thread.");

    global_osd_thread_signal = true;
    while (global_osd_thread_signal) {
        for (auto v : global_video)
        {
            if (v != nullptr)
            {
                if (v->active)
                {
                    if ((v->imp_encoder->osd != nullptr))
                    {
                        if (v->imp_encoder->osd->is_started)
                        {
                            v->imp_encoder->osd->updateDisplayEverySecond();
                        }
                        else
                        {
                            if (v->imp_encoder->osd->startup_delay)
                            {
                                v->imp_encoder->osd->startup_delay--;
                            }
                            else
                            {
                                v->imp_encoder->osd->start();
                            }
                        }
                    }
                }
            }
        }
        usleep(THREAD_SLEEP);
    }

    LOG_DEBUG("exit osd update thread.");
    return 0;
}
