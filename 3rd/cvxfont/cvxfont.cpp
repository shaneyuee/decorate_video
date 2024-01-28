#include "cvxfont.h"
#include <cassert>
#include <clocale>
#include <utility>
#include <sstream>
#include <cstdlib>

cvx::CvxFont::CvxFont(const cv::String& fontType)
{
    assert(!fontType.empty());
    m_error = FT_Init_FreeType(&m_library);
    if (m_error){
        std::cerr << "library initial error!" << std::endl;
        m_isValid = false;
        return;
    }
    m_error = FT_New_Face(m_library, fontType.c_str(), 0, &m_face);
    if (m_error == FT_Err_Unknown_File_Format){
        std::cerr << "unsupported font format!" << std::endl;
        m_isValid = false;
        return;
    }
    else if (m_error){
        std::cerr << " can not open font files" << std::endl;
        m_isValid = false;
        return;
    }
    // use default parameters
    m_isValid = true;
    m_font = new FontProperty;
    initFont();
    setlocale(LC_ALL, "");
}

// release freetype resource
cvx::CvxFont::~CvxFont()
{
    delete m_font;
    FT_Done_Face(m_face);
    FT_Done_FreeType(m_library);
}

void cvx::CvxFont::setFontSize(const int fontSize)
{
    m_font->fontSize = fontSize;
    FT_Set_Pixel_Sizes(m_face, fontSize, 0);
}

// initial font
void cvx::CvxFont::initFont()
{
    setSpaceRatio(0.5);
    setFontRatio(0);
    setRotateAngle(0);
    setDiaphaneity(1);
    setUnderline(false);
    setVertical(false);
    // set font
    FT_Set_Pixel_Sizes(m_face, getFontSize(), 0);
}

void cvx::CvxFont::rotateFont(double angle) {
    angle = (angle / 360) * 3.14159 * 2;
    /* set up matrix */
    m_matrix.xx = static_cast<FT_Fixed>(cos(angle) * 0x10000L);
    m_matrix.xy = static_cast<FT_Fixed>(-sin(angle) * 0x10000L);
    m_matrix.yx = static_cast<FT_Fixed>(sin(angle) * 0x10000L);
    m_matrix.yy = static_cast<FT_Fixed>(cos(angle) * 0x10000L);

    FT_Set_Transform(m_face, &m_matrix, nullptr);
}

// image could be empty to evaluate the size needed
void cvx::CvxFont::putTextStr(cv::Mat& img, const char* text, cv::Point& pos, const cv::Scalar& color, const char **endstr)
{
    CV_Assert(text && *text);

    int xStart = pos.x;
    int yStart = pos.y;
    m_newlinePos = pos;
    m_maxDiffHeight = 0;

    const char* ptr = text;
    std::mbtowc(nullptr, nullptr, 0); // reset the conversion state
    const char* end = ptr + strlen(ptr);
    int ret;

    cv::Point p = pos;
    for (wchar_t wc; (ret = std::mbtowc(&wc, ptr, end - ptr)) > 0; ptr += ret) {
        if (!putWChar(img, (wc & 0xffffffff), pos, color, endstr==NULL, p))
        {
            break;
        }
    }
    if (getVertical())
        m_newlinePos.x = p.x;
    else
        m_newlinePos.y = p.y;

    int xEnd = pos.x;
    int yEnd = pos.y;
    if (getUnderline() && img.cols) {
        if (getVertical()) {
            cv::line(img, cv::Point(xStart + m_maxDiffHeight, yStart), cv::Point(xStart + m_maxDiffHeight, yEnd), color, 2);
        }
        else {
            cv::line(img, cv::Point(xStart, yStart + m_maxDiffHeight), cv::Point(xEnd, yStart + m_maxDiffHeight), color, 2);
        }
    }

    if (endstr)
    {
        *endstr = (*ptr==0 || ret <= 0)? NULL : ptr;
    }
}

enum CHAR_ALIGN
{
    CHAR_TOP = 0,
    CHAR_UPPER = 1,
    CHAR_MIDDLE = 2,
    CHAR_BOTTOM = 3,
};

static CHAR_ALIGN getCharAlignment(uint32_t wc)
{
    static char lowList[] = "_,.;。，；、．";
    static char higList[] = "acegmnopqrsuvwxyz";
    static char midList[] = "-—=+~*·〇一二三四五六七八九十…—～∶:≈≡＝≠≤＜≮≯∷∞∝÷×－＋±∪∩∈∵∴√∽≌⊙⌒∠∥⊥0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static unsigned int lowWCList[sizeof(lowList)] = {0};
    static unsigned int higWCList[sizeof(higList)] = {0};
    static unsigned int midWCList[sizeof(midList)] = {0};

    if (!lowWCList[0])
    {
        char *ptr = lowList, *end = lowList + strlen(lowList);
        int nr = 0, ret;
        for (wchar_t wc; (ret = std::mbtowc(&wc, ptr, end - ptr)) > 0; ptr += ret)
        {
            lowWCList[nr++] = (wc & 0xffffffff);
        }
        while (nr < sizeof(lowList))
        {
            lowWCList[nr ++] = 0;
        }
    }
    if (!higWCList[0])
    {
        char *ptr = higList, *end = higList + strlen(higList);
        int nr = 0, ret;
        for (wchar_t wc; (ret = std::mbtowc(&wc, ptr, end - ptr)) > 0; ptr += ret)
        {
            higWCList[nr++] = (wc & 0xffffffff);
        }
        while (nr < sizeof(higList))
        {
            higWCList[nr ++] = 0;
        }
    }
    if (!midWCList[0])
    {
        char *ptr = midList, *end = midList + strlen(midList);
        int nr = 0, ret;
        for (wchar_t wc; (ret = std::mbtowc(&wc, ptr, end - ptr)) > 0; ptr += ret)
        {
            midWCList[nr++] = (wc & 0xffffffff);
        }
        while (nr < sizeof(midList))
        {
            midWCList[nr ++] = 0;
        }
    }
    for (int i=0; i<sizeof(lowList) && lowWCList[i]; i++)
        if (lowWCList[i] == wc)
            return CHAR_BOTTOM;
    for (int i=0; i<sizeof(higList) && higWCList[i]; i++)
        if (higWCList[i] == wc)
            return CHAR_UPPER;
    for (int i=0; i<sizeof(midList) && midWCList[i]; i++)
        if (midWCList[i] == wc)
            return CHAR_MIDDLE;
    return CHAR_TOP;
}

bool cvx::CvxFont::putWChar(cv::Mat& img, uint32_t wc, cv::Point& pos, const cv::Scalar& color, bool draw_partial, cv::Point& newlinePos)
{
    rotateFont(getAngle());
    const auto vertical = getVertical();
    const auto size = getFontSize();

    // Converting a Character Code Into a Glyph Index
    FT_UInt glyph_index = FT_Get_Char_Index(m_face, wc);
    FT_Load_Glyph(m_face, glyph_index, FT_LOAD_DEFAULT);
    FT_Render_Glyph(m_face->glyph, FT_RENDER_MODE_MONO);

    FT_GlyphSlot slot = m_face->glyph;
    FT_Bitmap bitmap = slot->bitmap;
    bool isSpace = wc == ' ';

    // get rows and cols of current wide char
    auto rows = bitmap.rows;
    auto cols = bitmap.width;
    const auto space = static_cast<int>(size * getSpaceRatio());
    const auto sep = static_cast<int>(size * getFontRatio());

    int alignOffset = 0;
    if (!vertical)
    {
        CHAR_ALIGN align = getCharAlignment(wc);
        if (align == CHAR_UPPER)
        {
            alignOffset += size * 3 / 10;
        }
        else if (align == CHAR_MIDDLE)
        {
            alignOffset += (size - rows) / 2;
        }
        else if (align == CHAR_BOTTOM)
        {
            alignOffset += (size * 9) / 10 - rows;
        }
    }

    cv::Point gPos = pos;
    //gPos.y += m_font->fontSize;
    if (vertical)
    {
        gPos.x += (slot->metrics.vertBearingX >> 6);
        gPos.y += (slot->metrics.vertBearingY >> 6);
        m_maxDiffHeight = std::max(m_maxDiffHeight, rows - (slot->metrics.vertBearingY >> 6));
    }
    else
    {
        gPos.x += (slot->metrics.horiBearingX >> 6);
    //    gPos.y -= (slot->metrics.horiBearingY >> 6); // ??
    //    gPos.y += (slot->metrics.horiBearingY >> 6);
        m_maxDiffHeight = std::max(m_maxDiffHeight, rows - (slot->metrics.horiBearingY >> 6));
    }

    bool result = true;
    if (pos.x + cols + m_maxDiffHeight >= img.cols ||
        pos.y + rows + m_maxDiffHeight >= img.rows)
    {
        if (!draw_partial) // not allowing partial character
        {
            result = false;
            goto __ret;
        }
    }

    // https://stackoverflow.com/questions/52254639/how-to-access-pixels-state-in-monochrome-bitmap-using-freetype2
    for (auto i = 0; i < rows; ++i)
    {
        for (auto j = 0; j < cols; ++j)
        {
            int off = i * slot->bitmap.pitch + j / 8;

            if (slot->bitmap.buffer[off] & (0x80 >> (j % 8)))
            {
                const auto r = gPos.y + i + alignOffset; // vertical ? pos.y + i : pos.y + i + (size - rows); // to make align to bottom
                const auto c = gPos.x + j;

                if (r >= 0 && r < img.rows && c >= 0 && c < img.cols)
                {
                    cv::Vec4b scalar = img.at<cv::Vec4b>(cv::Point(c, r));

                    // merge set color with origin color
                    double p = getDiaphaneity();
                    for (int k = 0; k < 3; ++k)
                    {
                        scalar.val[k] = static_cast<uchar>(scalar.val[k] * (1 - p) + color.val[k] * p);
                    }
                    scalar.val[3] = color.val[3];
                    img.at<cv::Vec4b>(cv::Point(c, r)) = cv::Vec4b(scalar[0], scalar[1], scalar[2], scalar[3]);
                }
            }
        }
    }
    // modify position to next character
    if (vertical){ // vertical string or not, default not vertical
        const auto moveX = (static_cast<int>(getAngle()) == 0) ?  (slot->metrics.vertAdvance >> 6) : rows + 1;
        pos.y += isSpace ? space : moveX + sep;
    }
    else
    {
        const auto moveY = (static_cast<int>(getAngle()) == 0) ? (slot->metrics.horiAdvance >> 6) : cols + 1;
        pos.x += isSpace ? space : moveY + sep;
    }

__ret:
    // always return recommanded new line point
    if (vertical) // advance x, keep y
    {
        const auto moveY = (static_cast<int>(getAngle()) != 0) ? (slot->metrics.horiAdvance >> 6) : cols + 1;
        newlinePos.y = 0;
        newlinePos.x = gPos.x + (slot->metrics.vertBearingX >> 6) + (isSpace ? space : moveY + sep);
    }
    else // horizontal, advance y, keep x
    {
        const auto moveX = (static_cast<int>(getAngle()) != 0) ?  (slot->metrics.vertAdvance >> 6) : rows + 1;
        newlinePos.x = 0;
        newlinePos.y = gPos.y + (slot->metrics.horiBearingY >> 6) + (isSpace ? space : moveX + sep) ;
    }
    return result;
}

bool cvx::putText(cv::Mat& img, const std::string& text, cv::Point& pos, cvx::CvxFont& fontFace, int fontSize, const cv::Scalar& color, MatTextWrapMode wrapMode)
{
    fontFace.setFontSize(fontSize);
    if (wrapMode == WRAP_CROP)
    {
        const char *end = NULL;
        fontFace.putTextStr(img, text.c_str(), pos, color, &end);
        return end == NULL;
    }

    cv::Point prevPos = pos, originPos = pos;
    const char *end = text.c_str();
    const int maxFontHeight = 200;
    cv::Point newlinePos = pos;
    auto vertical = fontFace.getVertical();

    while (end) // wrap to new line
    {
        if (newlinePos.x + 10 > img.cols || newlinePos.y + 10 > img.rows) // not enough space to draw
            return false;
        cv::Size newSize;
        cv::Point newPos(0,0);
        if (vertical)
        {
            int defaultWidth = img.cols > maxFontHeight? maxFontHeight : img.cols;
            newSize = cv::Size(newlinePos.x > prevPos.x? newlinePos.x - prevPos.x : defaultWidth, img.rows-originPos.y*2);
        }
        else
        {
            int defaultHeight = img.rows > maxFontHeight? maxFontHeight : img.rows;
            newSize = cv::Size(img.cols-originPos.x*2, newlinePos.y > prevPos.y? newlinePos.y - prevPos.y : defaultHeight);
        }
        cv::Mat mat(newSize, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        const char *str = end;
        end = NULL;
        fontFace.putTextStr(mat, str, newPos, color, &end);

        if (end == str) // nothing drawn
            return false;

        // merge mat to img with alignment specified in wrapMode
        cv::Rect newRect(0,0,0,0), oldRect;
        if (vertical)
        {
            if (newSize.width > img.cols)
                newSize.width = img.cols;
            if (newPos.y > img.rows)
                newPos.y = img.rows;
            newRect.width = oldRect.width = newSize.width;
            newRect.height = oldRect.height = newPos.y;
            oldRect.x = newlinePos.x;
            if (wrapMode == WRAP_ALIGN_CENTER)
                oldRect.y = (img.rows - newRect.height) / 2;
            else
                oldRect.y = originPos.y;
            if (oldRect.x + oldRect.width > img.cols)
            {
                newRect.width = oldRect.width = img.cols - oldRect.x;
            }
        }
        else
        {
            if (newPos.x > img.cols)
                newPos.x = img.cols;
            if (newSize.height > img.rows)
                newSize.height = img.rows;
            newRect.width = oldRect.width = newPos.x;
            newRect.height = oldRect.height = newSize.height;
            if (wrapMode == WRAP_ALIGN_CENTER)
                oldRect.x = (img.cols - newRect.width) / 2;
            else
                oldRect.x = originPos.x;
            oldRect.y = newlinePos.y;
            if (oldRect.y + oldRect.height > img.rows)
            {
                newRect.height = oldRect.height = img.rows - oldRect.y;
            }
        }
        if (newRect.width > 0 || newRect.height > 0)
        {
            mat(newRect).copyTo(img(oldRect));

        }

        // update newlinePos
        cv::Point newlinePos2 = fontFace.getNewlinePos();
        newlinePos.x += newlinePos2.x;
        newlinePos.y += newlinePos2.y;
        if (newlinePos.x > img.cols)
            newlinePos.x = img.cols;
        if (newlinePos.y > img.rows)
            newlinePos.y = img.rows;
        fontFace.setNewlinePos(newlinePos);

        // update pos
        if (vertical)
            pos = cv::Point(oldRect.x, originPos.y + oldRect.height);
        else
            pos = cv::Point(originPos.x + oldRect.width, oldRect.y);
        if (pos.x > img.cols)
            pos.x = img.cols;
        if (pos.y > img.rows)
            pos.y = img.rows;

        if ((vertical && newlinePos.x >= img.cols) ||  // out of bondary
                (!vertical && newlinePos.y >= img.rows))
            break;
    }
    return true;
}
