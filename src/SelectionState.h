#pragma once

#include <d2d1.h>

#include <algorithm>
#include <cmath>

enum class SelectionHit
{
    None,
    Inside,
    NW,
    N,
    NE,
    E,
    SE,
    S,
    SW,
    W,
};

// Rubber-band selection geometry in image coordinates: create by dragging,
// resize via the 8 handles (corners and anywhere along an edge), move by
// dragging the interior. Pure math; input routing and rendering live in
// MainWindow.
class SelectionState
{
public:
    bool HasRect() const { return m_hasRect; }
    bool Dragging() const { return m_drag != DragKind::None; }
    D2D1_RECT_F Rect() const { return m_rect; }

    void Reset()
    {
        m_hasRect = false;
        m_drag = DragKind::None;
    }

    // tolerance is in image pixels (screen handle size divided by zoom).
    SelectionHit HitTest(D2D1_POINT_2F pt, float tolerance) const
    {
        if (!m_hasRect)
        {
            return SelectionHit::None;
        }
        // "near" itself is a legacy windows.h macro, hence the name.
        const auto isNear = [tolerance](float a, float b) { return std::abs(a - b) <= tolerance; };
        const bool inX = pt.x >= m_rect.left - tolerance && pt.x <= m_rect.right + tolerance;
        const bool inY = pt.y >= m_rect.top - tolerance && pt.y <= m_rect.bottom + tolerance;

        // Corners take precedence over edges.
        if (isNear(pt.x, m_rect.left) && isNear(pt.y, m_rect.top))
        {
            return SelectionHit::NW;
        }
        if (isNear(pt.x, m_rect.right) && isNear(pt.y, m_rect.top))
        {
            return SelectionHit::NE;
        }
        if (isNear(pt.x, m_rect.right) && isNear(pt.y, m_rect.bottom))
        {
            return SelectionHit::SE;
        }
        if (isNear(pt.x, m_rect.left) && isNear(pt.y, m_rect.bottom))
        {
            return SelectionHit::SW;
        }
        if (isNear(pt.y, m_rect.top) && inX)
        {
            return SelectionHit::N;
        }
        if (isNear(pt.y, m_rect.bottom) && inX)
        {
            return SelectionHit::S;
        }
        if (isNear(pt.x, m_rect.left) && inY)
        {
            return SelectionHit::W;
        }
        if (isNear(pt.x, m_rect.right) && inY)
        {
            return SelectionHit::E;
        }
        if (pt.x > m_rect.left && pt.x < m_rect.right && pt.y > m_rect.top
            && pt.y < m_rect.bottom)
        {
            return SelectionHit::Inside;
        }
        return SelectionHit::None;
    }

    void BeginDrag(SelectionHit hit, D2D1_POINT_2F pt)
    {
        switch (hit)
        {
        case SelectionHit::Inside:
            m_drag = DragKind::Move;
            m_grabOffset = {pt.x - m_rect.left, pt.y - m_rect.top};
            break;
        case SelectionHit::NW:
            BeginResizeBoth({m_rect.right, m_rect.bottom});
            break;
        case SelectionHit::NE:
            BeginResizeBoth({m_rect.left, m_rect.bottom});
            break;
        case SelectionHit::SE:
            BeginResizeBoth({m_rect.left, m_rect.top});
            break;
        case SelectionHit::SW:
            BeginResizeBoth({m_rect.right, m_rect.top});
            break;
        case SelectionHit::N:
            m_drag = DragKind::ResizeY;
            m_anchor.y = m_rect.bottom;
            break;
        case SelectionHit::S:
            m_drag = DragKind::ResizeY;
            m_anchor.y = m_rect.top;
            break;
        case SelectionHit::W:
            m_drag = DragKind::ResizeX;
            m_anchor.x = m_rect.right;
            break;
        case SelectionHit::E:
            m_drag = DragKind::ResizeX;
            m_anchor.x = m_rect.left;
            break;
        case SelectionHit::None:
        default:
            m_drag = DragKind::Create;
            m_anchor = pt;
            m_rect = {pt.x, pt.y, pt.x, pt.y};
            m_hasRect = true;
            break;
        }
    }

    void UpdateDrag(D2D1_POINT_2F pt, float imageWidth, float imageHeight)
    {
        pt.x = std::clamp(pt.x, 0.0f, imageWidth);
        pt.y = std::clamp(pt.y, 0.0f, imageHeight);

        switch (m_drag)
        {
        case DragKind::Create:
        case DragKind::ResizeBoth:
            m_rect.left = std::min(m_anchor.x, pt.x);
            m_rect.right = std::max(m_anchor.x, pt.x);
            m_rect.top = std::min(m_anchor.y, pt.y);
            m_rect.bottom = std::max(m_anchor.y, pt.y);
            break;
        case DragKind::ResizeX:
            m_rect.left = std::min(m_anchor.x, pt.x);
            m_rect.right = std::max(m_anchor.x, pt.x);
            break;
        case DragKind::ResizeY:
            m_rect.top = std::min(m_anchor.y, pt.y);
            m_rect.bottom = std::max(m_anchor.y, pt.y);
            break;
        case DragKind::Move:
        {
            const float width = m_rect.right - m_rect.left;
            const float height = m_rect.bottom - m_rect.top;
            const float x =
                std::clamp(pt.x - m_grabOffset.x, 0.0f, std::max(0.0f, imageWidth - width));
            const float y =
                std::clamp(pt.y - m_grabOffset.y, 0.0f, std::max(0.0f, imageHeight - height));
            m_rect = {x, y, x + width, y + height};
            break;
        }
        case DragKind::None:
        default:
            break;
        }
    }

    void EndDrag() { m_drag = DragKind::None; }

private:
    enum class DragKind
    {
        None,
        Create,
        Move,
        ResizeBoth,  // corner: both axes follow the pointer
        ResizeX,     // vertical edge
        ResizeY,     // horizontal edge
    };

    void BeginResizeBoth(D2D1_POINT_2F anchor)
    {
        m_drag = DragKind::ResizeBoth;
        m_anchor = anchor;
    }

    bool m_hasRect = false;
    D2D1_RECT_F m_rect{};
    DragKind m_drag = DragKind::None;
    D2D1_POINT_2F m_anchor{};      // fixed corner / edge coordinate
    D2D1_POINT_2F m_grabOffset{};  // Move: pointer offset from rect origin
};
