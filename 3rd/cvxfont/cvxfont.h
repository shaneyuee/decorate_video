#ifndef OPENCVUNICODE_CVXFONT_H
#define OPENCVUNICODE_CVXFONT_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include <opencv2/opencv.hpp>

#include <typeinfo>
#include <codecvt>
#include <string>
#include <locale>

namespace cvx {
    enum MatTextWrapMode {
        WRAP_CROP = 0, // text outside the max_width will be cropped
        WRAP_ALIGN_LEFT = 1, // wrap to a new line, aligned to left
        WRAP_ALIGN_CENTER = 2,
    };

    struct FontProperty {
        int fontSize;           // font size (pixel)
        double spaceRatio;       // ratio of distance when meet a space, base on font size
        double fontRatio;        // ratio of distance between each character, base on font size
        double fontRotateAngle;  // rotate angle
        double fontDiaphaneity;  // merge ratio
        bool fontIsUnderline;   // underline
        bool fontIsVertical;    // put text in vertical
    };

    class CvxFont
    {
    public:

        explicit CvxFont(const cv::String& fontType);
        virtual ~CvxFont();

        void setFontSize(int fontSize);
        void setSpaceRatio(const double spaceRatio) { m_font->spaceRatio = spaceRatio; }
        void setFontRatio(const double fontRatio) { m_font->fontRatio = fontRatio; }
        void setRotateAngle(const double angle) { m_font->fontRotateAngle = angle; }
        void setUnderline(const bool isUnderline) { m_font->fontIsUnderline = isUnderline; }
        void setDiaphaneity(const double diaphaneity) { m_font->fontDiaphaneity = diaphaneity; }
        void setVertical(const bool vertical) { m_font->fontIsVertical = vertical; }
        void setNewlinePos(const cv::Point& p) { m_newlinePos = p; }

        [[nodiscard]] int getFontSize() const { return m_font->fontSize; }
        [[nodiscard]] double getSpaceRatio() const { return m_font->spaceRatio; }
        [[nodiscard]] double getFontRatio() const { return m_font->fontRatio; }
        [[nodiscard]] double getAngle() const { return m_font->fontRotateAngle; }
        [[nodiscard]] bool getUnderline() const { return m_font->fontIsUnderline; }
        [[nodiscard]] double getDiaphaneity() const { return m_font->fontDiaphaneity; }
        [[nodiscard]] bool getVertical() const { return m_font->fontIsVertical; }
        [[nodiscard]] int getDisplayHeight() const { return m_maxDiffHeight; }
        [[nodiscard]] cv::Point getNewlinePos() const { return m_newlinePos; }
        [[nodiscard]] bool isValid() const { return m_isValid; }

        void initFont();
        void putTextStr(cv::Mat& img, const char* text, cv::Point& pos, const cv::Scalar& color, const char **end);

    private:
        void rotateFont(double angle);
        bool putWChar(cv::Mat& img, uint32_t wc, cv::Point& pos, const cv::Scalar& color, bool draw_partial, cv::Point& newlinePos);
        FT_Library   m_library{};   // font library
        FT_Face      m_face{};      // font type
        FT_Matrix    m_matrix{};
        FT_Vector    m_pen{};
        FT_Error     m_error;

        FontProperty* m_font;
        long m_maxDiffHeight{ 0 };
        cv::Point m_newlinePos;
        bool m_isValid;
    };

    // return value:
    //    true - no text is cropped
    //    false - space is not enough and text is cropped
    bool putText(cv::Mat& img, const std::string& text, cv::Point& pos, cvx::CvxFont& fontFace, int fontSize, const cv::Scalar& color, MatTextWrapMode wrapMode);
}

#endif //OPENCVUNICODE_CVXFONT_H

