/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- adaptDispatch.hpp

Abstract:
- This serves as the Windows Console API-specific implementation of the callbacks from our generic Virtual Terminal parser.

Author(s):
- Michael Niksa (MiNiksa) 30-July-2015
--*/

#pragma once

#include "termDispatch.hpp"
#include "ITerminalApi.hpp"
#include "FontBuffer.hpp"
#include "MacroBuffer.hpp"
#include "PageManager.hpp"
#include "terminalOutput.hpp"

#include <unordered_map>
#include <map>
#include <deque>
#include <optional>
#include <algorithm>
#include "../input/terminalInput.hpp"
#include "../../types/inc/sgrStack.hpp"

// fwdecl unittest classes
#ifdef UNIT_TESTING
class AdapterTest;
#endif

namespace Microsoft::Console::VirtualTerminal
{
    class AdaptDispatch : public ITermDispatch
    {
        using Renderer = Microsoft::Console::Render::Renderer;
        using RenderSettings = Microsoft::Console::Render::RenderSettings;

    public:
        AdaptDispatch(ITerminalApi& api, Renderer* renderer, RenderSettings& renderSettings, TerminalInput& terminalInput) noexcept;

        void UnknownSequence() noexcept override;
        void Print(const wchar_t wchPrintable) override;
        void PrintString(const std::wstring_view string) override;

        void CursorUp(const VTInt distance) override; // CUU
        void CursorDown(const VTInt distance) override; // CUD
        void CursorForward(const VTInt distance) override; // CUF
        void CursorBackward(const VTInt distance) override; // CUB, BS
        void CursorNextLine(const VTInt distance) override; // CNL
        void CursorPrevLine(const VTInt distance) override; // CPL
        void CursorHorizontalPositionAbsolute(const VTInt column) override; // HPA, CHA
        void VerticalLinePositionAbsolute(const VTInt line) override; // VPA
        void HorizontalPositionRelative(const VTInt distance) override; // HPR
        void VerticalPositionRelative(const VTInt distance) override; // VPR
        void CursorPosition(const VTInt line, const VTInt column) override; // CUP, HVP
        void CursorSaveState() override; // DECSC
        void CursorRestoreState() override; // DECRC
        void EraseInDisplay(const DispatchTypes::EraseType eraseType) override; // ED
        void EraseInLine(const DispatchTypes::EraseType eraseType) override; // EL
        void EraseCharacters(const VTInt numChars) override; // ECH
        void SelectiveEraseInDisplay(const DispatchTypes::EraseType eraseType) override; // DECSED
        void SelectiveEraseInLine(const DispatchTypes::EraseType eraseType) override; // DECSEL
        void InsertCharacter(const VTInt count) override; // ICH
        void DeleteCharacter(const VTInt count) override; // DCH
        void ChangeAttributesRectangularArea(const VTInt top, const VTInt left, const VTInt bottom, const VTInt right, const VTParameters attrs) override; // DECCARA
        void ReverseAttributesRectangularArea(const VTInt top, const VTInt left, const VTInt bottom, const VTInt right, const VTParameters attrs) override; // DECRARA
        void CopyRectangularArea(const VTInt top, const VTInt left, const VTInt bottom, const VTInt right, const VTInt page, const VTInt dstTop, const VTInt dstLeft, const VTInt dstPage) override; // DECCRA
        void FillRectangularArea(const VTParameter ch, const VTInt top, const VTInt left, const VTInt bottom, const VTInt right) override; // DECFRA
        void EraseRectangularArea(const VTInt top, const VTInt left, const VTInt bottom, const VTInt right) override; // DECERA
        void SelectiveEraseRectangularArea(const VTInt top, const VTInt left, const VTInt bottom, const VTInt right) override; // DECSERA
        void SelectAttributeChangeExtent(const DispatchTypes::ChangeExtent changeExtent) noexcept override; // DECSACE
        void RequestChecksumRectangularArea(const VTInt id, const VTInt page, const VTInt top, const VTInt left, const VTInt bottom, const VTInt right) override; // DECRQCRA
        void SetGraphicsRendition(const VTParameters options) override; // SGR
        void SetLineRendition(const LineRendition rendition) override; // DECSWL, DECDWL, DECDHL
        void SetCharacterProtectionAttribute(const VTParameters options) override; // DECSCA
        void PushGraphicsRendition(const VTParameters options) override; // XTPUSHSGR
        void PopGraphicsRendition() override; // XTPOPSGR
        void DeviceStatusReport(const DispatchTypes::StatusType statusType, const VTParameter id) override; // DSR
        void DeviceAttributes() override; // DA1
        void SecondaryDeviceAttributes() override; // DA2
        void TertiaryDeviceAttributes() override; // DA3
        void Vt52DeviceAttributes() override; // VT52 Identify
        void RequestTerminalParameters(const DispatchTypes::ReportingPermission permission) override; // DECREQTPARM
        void ScrollUp(const VTInt distance) override; // SU
        void ScrollDown(const VTInt distance) override; // SD
        void NextPage(const VTInt pageCount) override; // NP
        void PrecedingPage(const VTInt pageCount) override; // PP
        void PagePositionAbsolute(const VTInt page) override; // PPA
        void PagePositionRelative(const VTInt pageCount) override; // PPR
        void PagePositionBack(const VTInt pageCount) override; // PPB
        void RequestDisplayedExtent() override; // DECRQDE
        void InsertLine(const VTInt distance) override; // IL
        void DeleteLine(const VTInt distance) override; // DL
        void InsertColumn(const VTInt distance) override; // DECIC
        void DeleteColumn(const VTInt distance) override; // DECDC
        void SetMode(const DispatchTypes::ModeParams param) override; // SM, DECSET
        void ResetMode(const DispatchTypes::ModeParams param) override; // RM, DECRST
        void RequestMode(const DispatchTypes::ModeParams param) override; // DECRQM
        void SetKeypadMode(const bool applicationMode) noexcept override; // DECKPAM, DECKPNM
        void SetAnsiMode(const bool ansiMode) override; // DECANM
        void SetKittyKeyboardProtocol(const VTParameter flags, const VTParameter mode) noexcept override; // KKP
        void QueryKittyKeyboardProtocol() override; // KKP
        void PushKittyKeyboardProtocol(const VTParameter flags) override; // KKP
        void PopKittyKeyboardProtocol(const VTParameter count) override; // KKP
        void SetTopBottomScrollingMargins(const VTInt topMargin,
                                          const VTInt bottomMargin) override; // DECSTBM
        void SetLeftRightScrollingMargins(const VTInt leftMargin,
                                          const VTInt rightMargin) override; // DECSLRM
        void EnquireAnswerback() override; // ENQ
        void WarningBell() override; // BEL
        void CarriageReturn() override; // CR
        void LineFeed(const DispatchTypes::LineFeedType lineFeedType) override; // IND, NEL, LF, FF, VT
        void ReverseLineFeed() override; // RI
        void BackIndex() override; // DECBI
        void ForwardIndex() override; // DECFI
        void SetWindowTitle(const std::wstring_view title) override; // DECSWT, OSCWindowTitle
        void SetCurrentWorkingDirectory(std::wstring_view uri) override; // OSC 7
        void HorizontalTabSet() override; // HTS
        void ForwardTab(const VTInt numTabs) override; // CHT, HT
        void BackwardsTab(const VTInt numTabs) override; // CBT
        void TabClear(const DispatchTypes::TabClearType clearType) override; // TBC
        void TabSet(const VTParameter setType) noexcept override; // DECST8C
        void DesignateCodingSystem(const VTID codingSystem) override; // DOCS
        void Designate94Charset(const VTInt gsetNumber, const VTID charset) override; // SCS
        void Designate96Charset(const VTInt gsetNumber, const VTID charset) override; // SCS
        void LockingShift(const VTInt gsetNumber) override; // LS0, LS1, LS2, LS3
        void LockingShiftRight(const VTInt gsetNumber) override; // LS1R, LS2R, LS3R
        void SingleShift(const VTInt gsetNumber) noexcept override; // SS2, SS3
        void AcceptC1Controls(const bool enabled) override; // DECAC1
        void SendC1Controls(const bool enabled) override; // S8C1T, S7C1T
        void AnnounceCodeStructure(const VTInt ansiLevel) override; // ACS
        void SoftReset() override; // DECSTR
        void HardReset(bool erase) override; // RIS
        void ScreenAlignmentPattern() override; // DECALN
        void SetCursorStyle(const DispatchTypes::CursorStyle cursorStyle) override; // DECSCUSR

        void SetClipboard(const wil::zwstring_view content) override; // OSCSetClipboard

        void SetColorTableEntry(const size_t tableIndex,
                                const DWORD color) override; // OSCSetColorTable
        void RequestColorTableEntry(const size_t tableIndex) override; // OSCGetColorTable
        void ResetColorTable() override; // OSCResetColorTable
        void ResetColorTableEntry(const size_t tableIndex) override; // OSCResetColorTable
        void SetXtermColorResource(const size_t resource, const DWORD color) override; // OSCSetDefaultForeground, OSCSetDefaultBackground, OSCSetCursorColor
        void RequestXtermColorResource(const size_t resource) override; // OSCGetDefaultForeground, OSCGetDefaultBackground, OSCGetCursorColor
        void ResetXtermColorResource(const size_t resource) override; // OSCResetForegroundColor, OSCResetBackgroundColor, OSCResetCursorColor, OSCResetHighlightColor
        void AssignColor(const DispatchTypes::ColorItem item, const VTInt fgIndex, const VTInt bgIndex) override; // DECAC

        void WindowManipulation(const DispatchTypes::WindowManipulationType function,
                                const VTParameter parameter1,
                                const VTParameter parameter2) override; // DTTERM_WindowManipulation

        void AddHyperlink(const std::wstring_view uri, const std::wstring_view params) override;
        void EndHyperlink() override;

        void DoConEmuAction(const std::wstring_view string) override;

        void DoITerm2Action(const std::wstring_view string) override;

        void DoFinalTermAction(const std::wstring_view string) override;

        void DoVsCodeAction(const std::wstring_view string) override;

        void DoWTAction(const std::wstring_view string) override;

        void DoUrxvtAction(const std::wstring_view string) override;

        StringHandler DefineSixelImage(const VTInt macroParameter,
                                       const DispatchTypes::SixelBackground backgroundSelect,
                                       const VTParameter backgroundColor) override; // SIXEL

        StringHandler KittyGraphics() override; // Kitty graphics protocol (APC G)

        StringHandler DownloadDRCS(const VTInt fontNumber,
                                   const VTParameter startChar,
                                   const DispatchTypes::DrcsEraseControl eraseControl,
                                   const DispatchTypes::DrcsCellMatrix cellMatrix,
                                   const DispatchTypes::DrcsFontSet fontSet,
                                   const DispatchTypes::DrcsFontUsage fontUsage,
                                   const VTParameter cellHeight,
                                   const DispatchTypes::CharsetSize charsetSize) override; // DECDLD

        void RequestUserPreferenceCharset() override; // DECRQUPSS
        StringHandler AssignUserPreferenceCharset(const DispatchTypes::CharsetSize charsetSize) override; // DECAUPSS

        StringHandler DefineMacro(const VTInt macroId,
                                  const DispatchTypes::MacroDeleteControl deleteControl,
                                  const DispatchTypes::MacroEncoding encoding) override; // DECDMAC
        void InvokeMacro(const VTInt macroId) override; // DECINVM

        void RequestTerminalStateReport(const DispatchTypes::ReportFormat format, const VTParameter formatOption) override; // DECRQTSR
        StringHandler RestoreTerminalState(const DispatchTypes::ReportFormat format) override; // DECRSTS

        StringHandler RequestSetting() override; // DECRQSS

        void RequestPresentationStateReport(const DispatchTypes::PresentationReportFormat format) override; // DECRQPSR
        StringHandler RestorePresentationState(const DispatchTypes::PresentationReportFormat format) override; // DECRSPS

        void PlaySounds(const VTParameters parameters) override; // DECPS

        void SetOptionalFeatures(const til::enumset<OptionalFeature> features) noexcept override;

    private:
        enum class Mode
        {
            InsertReplace,
            Origin,
            Column,
            AllowDECCOLM,
            AllowDECSLRM,
            SixelDisplay,
            EraseColor,
            RectangularChangeExtent,
            PageCursorCoupling
        };
        enum class ScrollDirection
        {
            Up,
            Down
        };
        struct CursorState
        {
            VTInt Row = 1;
            VTInt Column = 1;
            VTInt Page = 1;
            bool IsDelayedEOLWrap = false;
            bool IsOriginModeRelative = false;
            TextAttribute Attributes = {};
            TerminalOutput TermOutput = {};
        };
        struct Offset
        {
            VTInt Value;
            bool IsAbsolute;
            // VT origin is at 1,1 so we need to subtract 1 from absolute positions.
            static constexpr Offset Absolute(const VTInt value) { return { value - 1, true }; };
            static constexpr Offset Forward(const VTInt value) { return { value, false }; };
            static constexpr Offset Backward(const VTInt value) { return { -value, false }; };
            static constexpr Offset Unchanged() { return Forward(0); };
        };
        struct ChangeOps
        {
            CharacterAttributes andAttrMask = CharacterAttributes::All;
            CharacterAttributes xorAttrMask = CharacterAttributes::Normal;
            std::optional<TextColor> foreground;
            std::optional<TextColor> background;
            std::optional<TextColor> underlineColor;
        };

        void _WriteToBuffer(const std::wstring_view string);
        std::pair<int, int> _GetVerticalMargins(const Page& page, const bool absolute) noexcept;
        std::pair<int, int> _GetHorizontalMargins(const til::CoordType bufferWidth) noexcept;
        void _CursorMovePosition(const Offset rowOffset, const Offset colOffset, const bool clampInMargins);
        void _FillRect(const Page& page, const til::rect& fillRect, const std::wstring_view& fillChar, const TextAttribute& fillAttrs) const;
        void _SelectiveEraseRect(const Page& page, const til::rect& eraseRect);
        void _ChangeRectAttributes(const Page& page, const til::rect& changeRect, const ChangeOps& changeOps);
        void _ChangeRectOrStreamAttributes(const til::rect& changeArea, const ChangeOps& changeOps);
        til::rect _CalculateRectArea(const Page& page, const VTInt top, const VTInt left, const VTInt bottom, const VTInt right);
        void _EraseScrollback();
        void _EraseAll();
        TextAttribute _GetEraseAttributes(const Page& page) const noexcept;
        void _ScrollRectVertically(const Page& page, const til::rect& scrollRect, const VTInt delta);
        void _ScrollRectHorizontally(const Page& page, const til::rect& scrollRect, const VTInt delta);
        void _InsertDeleteCharacterHelper(const VTInt delta);
        void _InsertDeleteLineHelper(const VTInt delta);
        void _InsertDeleteColumnHelper(const VTInt delta);
        void _ScrollMovement(const VTInt delta);

        void _DoSetTopBottomScrollingMargins(const VTInt topMargin,
                                             const VTInt bottomMargin,
                                             const bool homeCursor = false);
        void _DoSetLeftRightScrollingMargins(const VTInt leftMargin,
                                             const VTInt rightMargin,
                                             const bool homeCursor = false);

        bool _DoLineFeed(const Page& page, const bool withReturn, const bool wrapForced);

        void _DeviceStatusReport(const wchar_t* parameters) const;
        void _CursorPositionReport(const bool extendedReport);
        void _MacroSpaceReport() const;
        void _MacroChecksumReport(const VTParameter id) const;

        void _SetColumnMode(const bool enable);
        void _SetAlternateScreenBufferMode(const bool enable);
        void _ModeParamsHelper(const DispatchTypes::ModeParams param, const bool enable);

        void _ClearSingleTabStop();
        void _ClearAllTabStops() noexcept;
        void _InitTabStopsForWidth(const VTInt width);

        void _ReportColorTable(const DispatchTypes::ColorModel colorModel) const;
        StringHandler _RestoreColorTable();

        void _ReportSGRSetting() const;
        void _ReportDECSTBMSetting();
        void _ReportDECSLRMSetting();
        void _ReportDECSCUSRSetting() const;
        void _ReportDECSCASetting() const;
        void _ReportDECSACESetting() const;
        void _ReportDECACSetting(const VTInt itemNumber) const;

        void _ReportCursorInformation();
        StringHandler _RestoreCursorInformation();
        void _ReportTabStops();
        StringHandler _RestoreTabStops();

        void _ReturnCsiResponse(const std::wstring_view response) const;
        void _ReturnDcsResponse(const std::wstring_view response) const;
        void _ReturnApcResponse(const std::wstring_view response) const;
        struct KittyControl
        {
            wchar_t action = L't';
            wchar_t deleteTarget = L'a';
            wchar_t compression = 0;
            uint32_t imageId = 0;
            uint32_t imageNumber = 0;
            uint32_t quiet = 0;
            uint32_t format = 32;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t cols = 0;    // c=: scale the placement to this many cell columns
            uint32_t rows = 0;    // r=: scale the placement to this many cell rows
            uint32_t srcX = 0;    // x=: source crop left edge in pixels
            uint32_t srcY = 0;    // y=: source crop top edge in pixels
            uint32_t srcW = 0;    // w=: source crop width in pixels (0 = to right edge)
            uint32_t srcH = 0;    // h=: source crop height in pixels (0 = to bottom edge)
            bool moreChunks = false;
            bool mPresent = false;
            bool haveId = false;
            bool haveNumber = false;
            wchar_t medium = L'd';          // t=: transmission medium (only d=direct in MVP)
            bool noCursorMovement = false;  // C=1: leave the cursor in place after a placement
            bool hasNonChunkKey = false;    // true if any key other than 'm' was present
            bool virtualPlacement = false;  // U=1: virtual placement (store only; drawn later via Unicode placeholders)
            // Relative placements (https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements).
            uint32_t placementId = 0;        // p=: placement id (one display of an image); ignored when imageId==0
            uint32_t parentImageId = 0;      // P=: parent image id this placement is positioned relative to
            uint32_t parentPlacementId = 0;  // Q=: parent placement id (with P) identifying the parent placement
            int32_t offsetH = 0;             // H=: signed horizontal cell offset from the parent anchor (+right)
            int32_t offsetV = 0;             // V=: signed vertical cell offset from the parent anchor (+down)
            bool havePlacementId = false;    // true if p= was present (so p= is echoed in the ack)
            bool haveParent = false;         // true if P= was present (a relative placement was requested)
        };
        // A stored Kitty image: the client image number (0 = none), the pixel
        // dimensions, and the decoded BGRA pixels (empty if not yet decodable).
        struct KittyImage
        {
            uint32_t number = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<RGBQUAD> pixels;
        };
        // The target pixel size a c=/r= request maps to (one axis preserves aspect). Shared
        // by the cursor-anchored and virtual paths so a U=1 grid matches an equivalent draw.
        struct KittyTargetSize
        {
            int64_t width = 0;
            int64_t height = 0;
        };
        // A virtual (U=1) placement's fixed grid geometry and source sampling state.
        // The grid (cols x rows) is recorded at store time so placeholder rendering slices
        // the image consistently no matter how the cells are chunked across writes.
        struct KittyVirtualPlacement
        {
            uint32_t cols = 1;
            uint32_t rows = 1;
            // Source crop rect (pixels) captured from x/y/w/h at store time (w/h=0/past-edge
            // already resolved and clamped to the image), so placeholder rendering samples the
            // same sub-rect a direct c/r placement would instead of the whole image. cropW/cropH
            // are always > 0 after _storeKittyVirtualPlacement (0 = unset => full image).
            uint32_t cropX = 0;
            uint32_t cropY = 0;
            uint32_t cropW = 0;
            uint32_t cropH = 0;
            // Exact scaled target pixel size (== _placeKittyImage's targetW/targetH). Placeholder
            // cells sample this continuous scaled space so a virtual grid is pixel-identical to a
            // direct c/r placement even for non-divisible geometry; pixels past it are the
            // aspect-preserving padding (transparent). 0 => fall back to gridCols/Rows * cell size.
            // 64-bit: aspect-preserving (c-only/r-only) scaling can exceed 2^32, and truncating it
            // would diverge the placeholder render from the direct one.
            uint64_t targetW = 0;
            uint64_t targetH = 0;
        };
        // A single placement (one display) of an image, identified by the (imageId, placementId)
        // pair. anchorRow/anchorCol are the absolute top-left cell of the placement. A relative
        // placement (hasParent) is positioned at parentAnchor + (offsetH, offsetV); a virtual
        // (isVirtual) placement has no fixed anchor and derives one on demand from its on-screen
        // Unicode-placeholder cells. Re-sending the same (imageId, placementId) replaces the entry.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        struct KittyPlacement
        {
            uint32_t imageId = 0;
            uint32_t placementId = 0;
            til::CoordType anchorRow = 0;
            til::CoordType anchorCol = 0;
            // The drawn footprint in cells (top-left at anchorCol/anchorRow), captured after
            // clamping so a re-put or cascade can erase exactly this placement's cells by rect.
            til::CoordType cols = 0;
            til::CoordType rows = 0;
            uint32_t parentImageId = 0;
            uint32_t parentPlacementId = 0;
            int32_t offsetH = 0;
            int32_t offsetV = 0;
            bool hasParent = false;
            bool isVirtual = false;
        };
        static KittyControl _ParseKittyControl(const std::wstring_view control) noexcept;
        void _HandleKittyGraphics(const std::wstring_view control, const std::string_view payload, const bool payloadValid, const bool payloadTooLarge);
        void _ProcessKittyCommand(const KittyControl& command, const std::string_view payload, const bool payloadValid, const bool payloadTooLarge);
        void _clearKittyChunk() noexcept;
        static uint32_t _ParseKittyUint(const std::wstring_view value) noexcept;
        static int32_t _ParseKittyInt(const std::wstring_view value) noexcept;
        static bool _DecodeKittyBase64(const std::string_view input, std::vector<uint8_t>& output) noexcept;
        static std::vector<RGBQUAD> _decodeKittyPixels(const uint32_t format, const std::vector<uint8_t>& bytes);
        uint32_t _kittyAssignImageId();
        void _registerKittyImage(const uint32_t id, KittyImage&& image);
        void _eraseKittyImage(const uint32_t id);
        void _eraseKittyImageRows(const uint32_t imageId);
        void _clearKittyImages() noexcept;
        void _storeKittyVirtualPlacement(const uint32_t id, const KittyImage& image, const uint32_t cols, const uint32_t rows, const uint32_t srcX, const uint32_t srcY, const uint32_t srcW, const uint32_t srcH);
        static KittyTargetSize _kittyTargetPixels(const int64_t cropW, const int64_t cropH, const uint32_t cols, const uint32_t rows, const int64_t cellWidth, const int64_t cellHeight) noexcept;
        til::size _placeKittyImage(const KittyImage& image, const bool moveCursor, const uint32_t imageId, const uint32_t cols = 0, const uint32_t rows = 0, const uint32_t srcX = 0, const uint32_t srcY = 0, const uint32_t srcW = 0, const uint32_t srcH = 0, const std::optional<til::point> anchor = std::nullopt);
        // Relative placement registry helpers.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        void _registerKittyPlacement(const KittyPlacement& placement);
        // Erases just one placement's drawn cells (by its tracked extent rect), leaving text and
        // co-resident images untouched -- a per-placement erase for a re-put (move/resize) or a
        // precise cascade delete, since image cells are owned by image id only, not (id, p).
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        void _eraseKittyPlacementCells(const KittyPlacement& placement);
        void _eraseKittyPlacementsForImage(const uint32_t imageId);
        // True if any tracked placement (registered or anonymous) still references this image id.
        bool _kittyImageHasPlacements(const uint32_t id) const noexcept;
        // Cascade-deletes the relative children of each removed placement key (registered +
        // anonymous), deleting any orphaned image except `keepImageId`, which the caller deletes.
        void _cascadeKittyPlacementChildren(std::deque<std::pair<uint32_t, uint32_t>>& removed, const uint32_t keepImageId);
        // Deletes only the (imageId, placementId) placement and its relative children, removing
        // imageId too if this was its last placement.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images
        void _deleteKittyPlacement(const uint32_t imageId, const uint32_t placementId);
        void _deleteKittyImagesIntersecting(const til::CoordType left, const til::CoordType top, const til::CoordType right, const til::CoordType bottom);
        void _deleteKittyImagesInIdRange(const uint32_t lo, const uint32_t hi);
        std::optional<til::point> _resolveKittyPlacementAnchor(const uint32_t parentImageId, const uint32_t parentPlacementId, const std::pair<uint32_t, uint32_t> origin, std::wstring_view& code) const;
        std::optional<til::point> _deriveVirtualPlacementAnchor(const uint32_t imageId) const;
        void _renderKittyPlaceholders(const std::wstring_view segment, const til::CoordType screenRow, const til::CoordType startColumn);
        // Returns true if a placeholder tile was drawn (the caller batches one redraw per segment).
        bool _placeKittyPlaceholderCell(const KittyImage& image, const uint32_t imageId, const til::CoordType column, const til::CoordType row, const uint32_t cellRow, const uint32_t cellCol, const KittyVirtualPlacement& place);
        static int _KittyPlaceholderDiacriticIndex(const char32_t ch) noexcept;
        void _ReturnOscResponse(const std::wstring_view response) const;

        std::vector<uint8_t> _tabStopColumns;
        bool _initDefaultTabStops = true;

        ITerminalApi& _api;
        Renderer* _renderer;
        RenderSettings& _renderSettings;
        TerminalInput& _terminalInput;
        TerminalOutput _termOutput;
        PageManager _pages;
        friend class SixelParser;
        std::shared_ptr<SixelParser> _sixelParser;
        std::unique_ptr<FontBuffer> _fontBuffer;
        std::shared_ptr<MacroBuffer> _macroBuffer;
        std::optional<unsigned int> _initialCodePage;
        til::enumset<OptionalFeature> _optionalFeatures = { OptionalFeature::ClipboardWrite };

        // Kitty graphics image registry. Each id maps to a KittyImage (number +
        // decoded BGRA pixels); a reverse number -> id map and FIFO/LRU eviction
        // bound the registry to MaxKittyImages entries and MaxKittyTotalBytes of
        // decoded pixels. The pixel cell size comes from the host (ITerminalApi).
        static constexpr size_t MaxKittyImages = 4096;
        static constexpr size_t MaxKittyPayload = 32 * 1024 * 1024;
        static constexpr size_t MaxKittyTotalBytes = 320 * 1024 * 1024;
        // The kitty Unicode placeholder code point. A cell holding this glyph, with a
        // 24-bit RGB foreground giving the image id, draws a sub-rect of a virtual
        // (U=1) image rather than the cursor-anchored placement.
        static constexpr wchar_t KittyPlaceholderCodePointHigh = 0xDBFB; // surrogate pair for U+10EEEE
        static constexpr wchar_t KittyPlaceholderCodePointLow = 0xDEEE;
        uint32_t _kittyNextImageId = 1;
        size_t _kittyTotalPixelBytes = 0;
        std::unordered_map<uint32_t, KittyImage> _kittyImages;
        std::unordered_map<uint32_t, uint32_t> _kittyImageNumbers;
        std::deque<uint32_t> _kittyImageOrder;
        // Ids placed virtually (U=1): only these may be drawn by U+10EEEE placeholders, so a
        // plain colored placeholder glyph can't false-overlay an ordinary image. The value is
        // the placement's fixed grid geometry and source sampling state (KittyVirtualPlacement).
        std::unordered_map<uint32_t, KittyVirtualPlacement> _kittyVirtualIds;
        // Placement registry, keyed by (imageId, placementId). Tracks each display of an image so
        // a relative child (P=/Q=) can be positioned against its parent and so a parent's deletion
        // cascades to its relative children. Bounded by MaxKittyImages-scale to match the image LRU.
        // A relative chain may be at least MaxKittyPlacementDepth deep; exceeding it is ETOODEEP.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        static constexpr int MaxKittyPlacementDepth = 8;
        static constexpr size_t MaxKittyPlacements = MaxKittyImages * 4;
        std::map<std::pair<uint32_t, uint32_t>, KittyPlacement> _kittyPlacements;
        // Anonymous relative placements (drawn with a parent but no placement id of their own).
        // They have no registry key, so they're tracked here purely so a parent's deletion can
        // cascade-erase them (spec: "If its parent is deleted, [it] is deleted as well."). Being
        // id-less they are always leaves -- they can never themselves be a parent.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        std::vector<KittyPlacement> _kittyAnonymousPlacements;

        // Chunked transmission (m=): accumulates the base64 payload across sequences;
        // only one transfer runs at a time, processed on the final chunk (m=0).
        bool _kittyChunkActive = false;
        bool _kittyChunkPayloadValid = true;
        bool _kittyChunkPayloadTooLarge = false;
        KittyControl _kittyChunkControl;
        std::string _kittyChunkPayload;

        // We have two instances of the saved cursor state, because we need
        // one for the main buffer (at index 0), and another for the alt buffer
        // (at index 1). The _usingAltBuffer property keeps tracks of which
        // buffer is active, so can be used as an index into this array to
        // obtain the saved state that should be currently active.
        std::array<CursorState, 2> _savedCursorState;
        bool _usingAltBuffer;

        til::inclusive_rect _scrollMargins;

        til::enumset<Mode> _modes{ Mode::PageCursorCoupling };

        SgrStack _sgrStack;

        void _SetUnderlineStyleHelper(const VTParameter option, TextAttribute& attr) noexcept;
        size_t _SetRgbColorsHelper(const VTParameters options,
                                   TextAttribute& attr,
                                   const bool isForeground) noexcept;
        void _SetRgbColorsHelperFromSubParams(const VTParameter colorItem,
                                              const VTSubParameters options,
                                              TextAttribute& attr) noexcept;
        size_t _ApplyGraphicsOption(const VTParameters options,
                                    const size_t optionIndex,
                                    TextAttribute& attr) noexcept;
        void _ApplyGraphicsOptionWithSubParams(const VTParameter option,
                                               const VTSubParameters subParams,
                                               TextAttribute& attr) noexcept;
        void _ApplyGraphicsOptions(const VTParameters options,
                                   TextAttribute& attr) noexcept;

#ifdef UNIT_TESTING
        friend class AdapterTest;
#endif
    };
}
