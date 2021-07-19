#include "CustomTextRenderer.h"
#include <vector>
#include <string>

using namespace DWriteColorTextRenderer;
using namespace Microsoft::WRL;
using std::vector;
using std::wstring;

// The constructor stores the Direct2D factory and device context
// and creates resources the renderer will use.
CustomTextRenderer::CustomTextRenderer(
    ComPtr<ID2D1Factory> d2dFactory,
    ComPtr<ID2D1RenderTarget> d2dDeviceContext
) :
    m_refCount(0),
    m_d2dFactory(d2dFactory),
    m_d2dDeviceContext(d2dDeviceContext)
{
    DX::ThrowIfFailed(
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory4),
            &m_dwriteFactory
        )
    );

    DX::ThrowIfFailed(
        m_d2dDeviceContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::Black),
            &m_outlineBrush
        )
    );

    DX::ThrowIfFailed(
        m_d2dDeviceContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White),
            &m_bodyBrush
        )
    );
}

// Decomposes the received glyph run into smaller color glyph runs
// using IDWriteFactory4::TranslateColorGlyphRun. Depending on the
// type of each color run, the renderer uses Direct2D to draw the
// outlines, SVG content, or bitmap content.
HRESULT CustomTextRenderer::DrawGlyphRun(
    _In_opt_ void* clientDrawingContext,
    FLOAT baselineOriginX,
    FLOAT baselineOriginY,
    DWRITE_MEASURING_MODE measuringMode,
    _In_ DWRITE_GLYPH_RUN const* glyphRun,
    _In_ DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
    IUnknown* clientDrawingEffect
)
{
    ComPtr<ID2D1PathGeometry> m_pPathGeometry;
    m_d2dFactory->CreatePathGeometry(&m_pPathGeometry);

    ComPtr<ID2D1GeometrySink> m_pGeometrySink;
    m_pPathGeometry->Open(&m_pGeometrySink);

    glyphRun->fontFace->GetGlyphRunOutline(
        glyphRun->fontEmSize,
        glyphRun->glyphIndices,
        glyphRun->glyphAdvances,
    	glyphRun->glyphOffsets,
        glyphRun->glyphCount,
    	glyphRun->isSideways,
        NULL,
    	m_pGeometrySink.Get());

    m_pGeometrySink->Close();

    auto transform = D2D1::Matrix3x2F::Translation(baselineOriginX, baselineOriginY);
    // transform = transform * D2D1::Matrix3x2F::Translation(0, -10);

    m_d2dDeviceContext->SetTransform(transform);
    // Draw text outline
    m_d2dDeviceContext->DrawGeometry(m_pPathGeometry.Get(), m_outlineBrush.Get(), 4);
    // Draw text body
    m_d2dDeviceContext->FillGeometry(m_pPathGeometry.Get(), m_bodyBrush.Get());

    //m_d2dDeviceContext->DrawGlyphRun(
    //    baselineOrigin,
    //    glyphRun,
    //    glyphRunDescription,
    //    m_bodyBrush.Get(),
    //    measuringMode
    //);

    return S_OK;
}

IFACEMETHODIMP CustomTextRenderer::DrawUnderline(
    _In_opt_ void* clientDrawingContext,
    FLOAT baselineOriginX,
    FLOAT baselineOriginY,
    _In_ DWRITE_UNDERLINE const* underline,
    IUnknown* clientDrawingEffect
)
{
    // Not implemented
    return E_NOTIMPL;
}

IFACEMETHODIMP CustomTextRenderer::DrawStrikethrough(
    _In_opt_ void* clientDrawingContext,
    FLOAT baselineOriginX,
    FLOAT baselineOriginY,
    _In_ DWRITE_STRIKETHROUGH const* strikethrough,
    IUnknown* clientDrawingEffect
)
{
    // Not implemented
    return E_NOTIMPL;
}

IFACEMETHODIMP CustomTextRenderer::DrawInlineObject(
    _In_opt_ void* clientDrawingContext,
    FLOAT originX,
    FLOAT originY,
    IDWriteInlineObject* inlineObject,
    BOOL isSideways,
    BOOL isRightToLeft,
    IUnknown* clientDrawingEffect
)
{
    // Not implemented
    return E_NOTIMPL;
}

IFACEMETHODIMP_(unsigned long) CustomTextRenderer::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

IFACEMETHODIMP_(unsigned long) CustomTextRenderer::Release()
{
    unsigned long newCount = InterlockedDecrement(&m_refCount);
    if (newCount == 0)
    {
        delete this;
        return 0;
    }

    return newCount;
}

IFACEMETHODIMP CustomTextRenderer::IsPixelSnappingDisabled(
    _In_opt_ void* clientDrawingContext,
    _Out_ BOOL* isDisabled
)
{
    *isDisabled = FALSE;
    return S_OK;
}

IFACEMETHODIMP CustomTextRenderer::GetCurrentTransform(
    _In_opt_ void* clientDrawingContext,
    _Out_ DWRITE_MATRIX* transform
)
{
    // forward the render target's transform
    m_d2dDeviceContext->GetTransform(reinterpret_cast<D2D1_MATRIX_3X2_F*>(transform));
    return S_OK;
}

IFACEMETHODIMP CustomTextRenderer::GetPixelsPerDip(
    _In_opt_ void* clientDrawingContext,
    _Out_ FLOAT* pixelsPerDip
)
{
    float x, yUnused;

    m_d2dDeviceContext.Get()->GetDpi(&x, &yUnused);
    *pixelsPerDip = x / 96.0f;

    return S_OK;
}

IFACEMETHODIMP CustomTextRenderer::QueryInterface(
    IID const& riid,
    void** ppvObject
)
{
    if (__uuidof(IDWriteTextRenderer) == riid)
    {
        *ppvObject = this;
    }
    else if (__uuidof(IDWritePixelSnapping) == riid)
    {
        *ppvObject = this;
    }
    else if (__uuidof(IUnknown) == riid)
    {
        *ppvObject = this;
    }
    else
    {
        *ppvObject = nullptr;
        return E_FAIL;
    }

    this->AddRef();

    return S_OK;
}