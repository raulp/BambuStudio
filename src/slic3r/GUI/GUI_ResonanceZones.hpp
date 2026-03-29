#ifndef slic3r_GUI_ResonanceZones_hpp_
#define slic3r_GUI_ResonanceZones_hpp_

#include "wxExtensions.hpp"
#include "../libslic3r/PrintConfig.hpp"
#include "Widgets/TextInput.hpp"
#include <wx/panel.h>
#include <atomic>

class wxBoxSizer;
class wxFlexGridSizer;

namespace Slic3r {
namespace GUI {

// Maximum number of resonance zones per extruder (0 = unlimited)
#define MAX_RESONANCE_ZONES 10

// ResonanceZone is defined in PrintConfig.hpp
using t_speed_range = ResonanceZone;

class ResonanceZones;

class SpeedRangeEditor : public ::TextInput
{
    bool      m_enter_pressed{false};
    wxString  m_valid_value;
    bool      m_is_min{true};

public:
    SpeedRangeEditor(ResonanceZones* parent,
                     const wxString& value,
                     bool is_min,
                     std::function<bool(double, bool)> edit_fn);
    ~SpeedRangeEditor() {}

    void     msw_rescale();

private:
    double   get_value() const;
};

class ResonanceZones : public wxPanel
{
public:
    using ZonesVec = std::vector<ResonanceZone>;

    struct ZoneValidationError {
        size_t zone_index;
        std::string error_message;
    };

    explicit ResonanceZones(wxWindow* parent);
    ~ResonanceZones() {}

    // Parent wiring
    void set_on_change(std::function<void()> cb);
    void set_on_empty(std::function<void()> cb);
    void set_extruder(int extruder_idx, ConfigOptionFloats* zones, bool visible);
    void reload();

    bool has_zones() const;

    // Operations
    void add_zone_after(size_t zone_index);
    void del_zone(const ResonanceZone& zone);
    bool edit_zone(const ResonanceZone& old_zone, const ResonanceZone& new_zone);

    // Validation
    std::vector<ZoneValidationError> validate_all_zones() const;

    // Sorting
    void sort_and_save_zones();  // Sort zones and save (used on load/save preset)

private:
    // Forward declare button class for use in ZoneRow struct
    class PlusMinusButton;

    // Widget tracking - each row contains widgets for one zone
    struct ZoneRow {
        ResonanceZone zone;                  // The data this row represents
        SpeedRangeEditor* min_editor;        // Min speed input
        SpeedRangeEditor* max_editor;        // Max speed input
        PlusMinusButton* del_button;         // Delete button
        PlusMinusButton* add_button;         // Add button
        wxBoxSizer* row_sizer;               // The horizontal sizer for the row
        wxSizer* button_sizer;               // The sizer for buttons

        ZoneRow() : min_editor(nullptr), max_editor(nullptr),
                   del_button(nullptr), add_button(nullptr),
                   row_sizer(nullptr), button_sizer(nullptr) {}
    };

    // Default initial zone range when resonance avoidance is first enabled
    static constexpr double DEFAULT_INITIAL_MIN_SPEED = 100.0;  // mm/s
    static constexpr double DEFAULT_INITIAL_MAX_SPEED = 130.0;  // mm/s

    wxWindow*           m_parent{nullptr};
    ScalableBitmap      m_bmp_delete;
    ScalableBitmap      m_bmp_add;
    wxBoxSizer*         m_main_sizer{nullptr};
    wxFlexGridSizer*    m_grid_sizer{nullptr};
    // LIFETIME: Points to data owned by Tab::m_config, must outlive this widget.
    // Parent Tab is responsible for ensuring config lifetime exceeds widget lifetime.
    ConfigOptionFloats* m_zones_config{nullptr};
    int                 m_extruder_idx{0};
    std::atomic<bool>   m_reload_pending{false};
    bool                m_pending_autoscroll{false};  // True when user clicks + to add zone
    std::optional<ResonanceZone> m_pending_highlight_zone;  // Zone to highlight after rebuild
    std::vector<ZoneRow> m_zone_rows;       // Track all current rows

    std::function<void()> m_on_change_callback;
    std::function<void()> m_on_empty_callback;

    // UI build
    ZoneRow   create_zone_row_tracked(const ResonanceZone& range, size_t zone_index, size_t total_zones);
    std::function<bool(double, bool)> create_min_editor_callback(size_t zone_index);
    std::function<bool(double, bool)> create_max_editor_callback(size_t zone_index);
    void      configure_button(PlusMinusButton* button, const wxString& tooltip, bool enable);
    void      update_zone_values();
    void      rebuild_all_rows();
    void      defer_rebuild();  // Helper to queue rebuild if not already pending
    void      update_parent_layout();  // Handle parent layout with size correction
    void      perform_autoscroll_to_bottom();  // Scroll to show newly added zone
    void      highlight_zone_row(size_t zone_index);  // Highlight and focus newly added zone
    void      clear_ui();
    void      reload_from_config();

    // Data helpers
    ZonesVec  get_zones() const;
    void      save_zones(const ZonesVec& zones);
    std::vector<ResonanceZone> get_zones_from_config() const;
    bool      is_valid_zone_index(size_t idx) const { return idx < m_zone_rows.size(); }
    ZonesVec  sanitize_zones(const ZonesVec& zones) const;  // Remove invalid/overlapping/duplicate zones

    // Error handling pattern:
    // - Internal validation helpers return bool + error string for testing/reuse
    // - Public operations (add_zone_after, del_zone) show wxMessageBox directly for immediate user feedback
    bool      validate_zone(const ResonanceZone& zone, std::string& error_msg, const ResonanceZone& exclude = ResonanceZone()) const;
    bool      check_overlap(const ResonanceZone& new_zone, const ResonanceZone& exclude = ResonanceZone()) const;

    // Progressive gap-filling helpers
    const ResonanceZone* find_next_zone_after(double max_speed, const ZonesVec& zones) const;
    void      calculate_zone_from_gap(double gap, double& buffer, double& width) const;

    // Button that remembers the speed range for which it was created
    class PlusMinusButton : public ScalableButton
    {
    public:
        PlusMinusButton(wxWindow* parent, const ScalableBitmap& bitmap, t_speed_range range)
            : ScalableButton(parent, wxID_ANY, bitmap), range(range) {}
        t_speed_range range;
    };

protected:
    // Override to always return correct size based on content
    virtual wxSize DoGetBestSize() const wxOVERRIDE;

public:
    void      msw_rescale();
    void      sys_color_changed();
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ResonanceZones_hpp_
