#include "GUI_ResonanceZones.hpp"

#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "Plater.hpp"
#include "I18N.hpp"

#include <wx/wupdlock.h>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <optional>
#include <limits>
#include <algorithm>
#include <cmath>
#include <cassert>

namespace Slic3r {
namespace GUI {

// SpeedRangeEditor

SpeedRangeEditor::SpeedRangeEditor(ResonanceZones* parent,
                                   const wxString& value,
                                   bool is_min,
                                   std::function<bool(double, bool)> edit_fn)
    : TextInput(parent, value, wxEmptyString, wxEmptyString, wxDefaultPosition,
                wxSize(12 * wxGetApp().em_unit(), -1), wxTE_PROCESS_ENTER, _L("mm/s"))
    , m_valid_value(value)
    , m_is_min(is_min)
{
    auto* ctrl = GetTextCtrl();
    wxTextValidator validator(wxFILTER_NUMERIC);
    ctrl->SetValidator(validator);

    // Disable autocomplete/autofill on macOS
    ctrl->SetHint(wxEmptyString);
#ifdef __WXMAC__
    ctrl->OSXDisableAllSmartSubstitutions();
#endif

    ctrl->Bind(wxEVT_TEXT_ENTER, [this, ctrl, edit_fn](wxEvent&) {
        m_enter_pressed = true;
        double v = get_value();
        if (v >= 0 && edit_fn(v, true)) {
            m_valid_value = double_to_string(v);
        } else {
            ctrl->ChangeValue(m_valid_value);
        }
        m_enter_pressed = false;
    }, ctrl->GetId());

    ctrl->Bind(wxEVT_KILL_FOCUS, [this, ctrl, edit_fn](wxFocusEvent& event) {
        event.Skip();
        if (!m_enter_pressed) {
            double v = get_value();
            if (v >= 0 && edit_fn(v, false)) {
                m_valid_value = double_to_string(v);
            } else {
                ctrl->ChangeValue(m_valid_value);
            }
        }
    }, ctrl->GetId());
}

double SpeedRangeEditor::get_value() const
{
    wxString str = GetTextCtrl()->GetValue();
    double value {0.0};
    str.ToDouble(&value);
    return value;
}

void SpeedRangeEditor::msw_rescale()
{
    SetMinSize(wxSize(12 * wxGetApp().em_unit(), -1));
}

// ResonanceZones

ResonanceZones::ResonanceZones(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_parent(parent)
{
    SetBackgroundColour(parent->GetBackgroundColour());

    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    m_grid_sizer = new wxFlexGridSizer(0, 2, em_unit(this) / 2, em_unit(this));
    m_grid_sizer->SetFlexibleDirection(wxBOTH);
    m_main_sizer->Add(m_grid_sizer, 0, wxEXPAND | wxALL, 0);
    SetSizer(m_main_sizer);

    // Use dark mode variants of bitmaps if in dark mode
    const std::string delete_icon = wxGetApp().dark_mode() ? "delete_filament_dark" : "delete_filament";
    const std::string add_icon = wxGetApp().dark_mode() ? "add_filament_dark" : "add_filament";
    m_bmp_delete = ScalableBitmap(parent, delete_icon);
    m_bmp_add    = ScalableBitmap(parent, add_icon);
}

void ResonanceZones::set_on_change(std::function<void()> cb) { m_on_change_callback = cb; }
void ResonanceZones::set_on_empty(std::function<void()> cb)  { m_on_empty_callback = cb; }

void ResonanceZones::clear_ui()
{
    m_zone_rows.clear();  // Clear tracking first
    m_grid_sizer->Clear(true);  // Then destroy widgets
}

void ResonanceZones::reload_from_config()
{
    auto zones = get_zones_from_config();
    size_t original_count = zones.size();

    // Sanitize: remove invalid zones, overlaps, and duplicates
    zones = sanitize_zones(zones);

    // If we cleaned up any zones, save the sanitized config back
    if (zones.size() < original_count) {
        BOOST_LOG_TRIVIAL(warning) << "Removed " << (original_count - zones.size())
                                   << " invalid/overlapping/duplicate resonance zones during config load";
        save_zones(zones);
    }

    // Check if zone count changed
    if (zones.size() != m_zone_rows.size()) {
        rebuild_all_rows();
    } else if (zones.size() > 0) {
        update_zone_values();
    } else {
        m_zone_rows.clear();
        m_grid_sizer->Clear(true);
    }
}

void ResonanceZones::reload()
{
    reload_from_config();
}

void ResonanceZones::update_zone_values()
{
    auto zones = get_zones_from_config();

    // Safety check: only update if zone count matches
    if (zones.size() != m_zone_rows.size()) {
        rebuild_all_rows();
        return;
    }

    // Update each row's values
    for (size_t i = 0; i < zones.size(); ++i) {
        const auto& zone = zones[i];
        auto& row = m_zone_rows[i];

        // Check if this row needs updating
        if (row.zone.min_speed != zone.min_speed ||
            row.zone.max_speed != zone.max_speed) {

            // Update stored zone data
            row.zone = zone;

            // Update min editor value without triggering events
            row.min_editor->GetTextCtrl()->ChangeValue(double_to_string(zone.min_speed));

            // Update max editor value without triggering events
            row.max_editor->GetTextCtrl()->ChangeValue(double_to_string(zone.max_speed));

            // Update button stored ranges (for event handlers)
            row.del_button->range = zone;
            row.add_button->range = zone;
        }
    }
}

std::function<bool(double, bool)> ResonanceZones::create_min_editor_callback(size_t zone_index)
{
    return [this, zone_index](double min_speed, bool enter_pressed) {
        if (!is_valid_zone_index(zone_index)) {
            return false;
        }
        const auto& current_zone = m_zone_rows[zone_index].zone;
        if (fabs(min_speed - current_zone.min_speed) < EPSILON) {
            return false;
        }
        double max_speed = min_speed < current_zone.max_speed
                         ? current_zone.max_speed
                         : min_speed + 10.0;
        ResonanceZone new_zone(min_speed, max_speed);
        return edit_zone(current_zone, new_zone);
    };
}

std::function<bool(double, bool)> ResonanceZones::create_max_editor_callback(size_t zone_index)
{
    return [this, zone_index](double max_speed, bool enter_pressed) {
        if (!is_valid_zone_index(zone_index)) {
            return false;
        }
        const auto& current_zone = m_zone_rows[zone_index].zone;
        if (fabs(max_speed - current_zone.max_speed) < EPSILON ||
            current_zone.min_speed > max_speed) {
            return false;
        }
        ResonanceZone new_zone(current_zone.min_speed, max_speed);
        return edit_zone(current_zone, new_zone);
    };
}

void ResonanceZones::configure_button(PlusMinusButton* button, const wxString& tooltip, bool enable)
{
    button->DisableFocusFromKeyboard();
    button->SetBackgroundColour(GetBackgroundColour());
    button->SetToolTip(tooltip);
    if (!enable) {
        button->Enable(false);
    }
}

ResonanceZones::ZoneRow ResonanceZones::create_zone_row_tracked(
    const ResonanceZone& range,
    size_t zone_index,
    size_t total_zones)
{
    ZoneRow tracked_row;
    tracked_row.zone = range;
    tracked_row.row_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Label with row number (right-aligned for consistent spacing)
    wxString label_text = wxString::Format("%2zu. %s", zone_index + 1, _L("Range:"));
    auto label = new wxStaticText(this, wxID_ANY, label_text, wxDefaultPosition, wxDefaultSize);
    label->SetBackgroundStyle(wxBG_STYLE_PAINT);
    label->SetFont(wxGetApp().normal_font());
    tracked_row.row_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em_unit(this));

    // Min editor
    tracked_row.min_editor = new SpeedRangeEditor(
        this, double_to_string(range.min_speed), true, create_min_editor_callback(zone_index));
    tracked_row.row_sizer->Add(tracked_row.min_editor, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em_unit(this));

    // "to" label
    auto middle = new wxStaticText(this, wxID_ANY, _L("to"), wxDefaultPosition, wxDefaultSize);
    middle->SetBackgroundStyle(wxBG_STYLE_PAINT);
    middle->SetFont(wxGetApp().normal_font());
    tracked_row.row_sizer->Add(middle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em_unit(this));

    // Max editor
    tracked_row.max_editor = new SpeedRangeEditor(
        this, double_to_string(range.max_speed), false, create_max_editor_callback(zone_index));
    tracked_row.row_sizer->Add(tracked_row.max_editor, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em_unit(this));

    // Buttons
    tracked_row.button_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Delete button
    tracked_row.del_button = new PlusMinusButton(this, m_bmp_delete, range);
    configure_button(tracked_row.del_button, _L("Remove zone"), true);
    tracked_row.button_sizer->Add(tracked_row.del_button, 0, wxRIGHT, em_unit(this) / 2);

    // Add button
    tracked_row.add_button = new PlusMinusButton(this, m_bmp_add, range);
    bool max_zones_reached = (MAX_RESONANCE_ZONES > 0 && total_zones >= MAX_RESONANCE_ZONES);
    wxString add_tooltip = max_zones_reached
        ? wxString::Format(_L("Maximum %d zones allowed"), MAX_RESONANCE_ZONES)
        : _L("Add zone after this one");
    configure_button(tracked_row.add_button, add_tooltip, !max_zones_reached);
    tracked_row.button_sizer->Add(tracked_row.add_button, 0, wxLEFT, em_unit(this) / 2);

    // Bind events
    tracked_row.del_button->Bind(wxEVT_BUTTON, [this, zone_index](wxEvent&) {
        if (is_valid_zone_index(zone_index)) {
            del_zone(m_zone_rows[zone_index].zone);
        }
    });
    tracked_row.add_button->Bind(wxEVT_BUTTON, [this, zone_index](wxEvent&) {
        add_zone_after(zone_index);
    });

    // Add to grid
    m_grid_sizer->Add(tracked_row.row_sizer, 0, wxALIGN_CENTER_VERTICAL);
    m_grid_sizer->Add(tracked_row.button_sizer, 0, wxALIGN_CENTER_VERTICAL);

    return tracked_row;
}

void ResonanceZones::rebuild_all_rows()
{
    // Clear widget tracking
    m_zone_rows.clear();

    // Destroy all widgets
    m_grid_sizer->Clear(true);

    // Get current zones from config
    auto zones = get_zones_from_config();

    // Create new rows
    m_zone_rows.reserve(zones.size());
    for (size_t i = 0; i < zones.size(); ++i) {
        m_zone_rows.push_back(create_zone_row_tracked(zones[i], i, zones.size()));
    }

    // Layout and fit
    m_grid_sizer->Layout();
    m_main_sizer->Fit(this);

    // Set minimum size so parent sizer can't shrink us to 0
    SetMinSize(GetSize());
    InvalidateBestSize();  // Force layout system to recalculate

    Layout();
}

void ResonanceZones::defer_rebuild()
{
    if (!m_reload_pending) {
        m_reload_pending = true;
        CallAfter([this]() {
            m_reload_pending = false;
            rebuild_all_rows();
        });
    }
}

bool ResonanceZones::has_zones() const
{
    return m_zones_config && !m_zones_config->values.empty();
}

std::vector<ResonanceZone> ResonanceZones::get_zones_from_config() const
{
    std::vector<ResonanceZone> zones;
    if (!m_zones_config) {
        return zones;
    }
    for (size_t i = 0; i + 1 < m_zones_config->values.size(); i += 2) {
        zones.emplace_back(m_zones_config->values[i], m_zones_config->values[i + 1]);
    }
    return zones;
}

void ResonanceZones::save_zones(const ZonesVec& zones)
{
    if (!m_zones_config) {
        return;
    }

    // Don't sort during editing - only sort on load/save preset
    m_zones_config->values.clear();
    for (const auto& z : zones) {
        m_zones_config->values.push_back(z.min_speed);
        m_zones_config->values.push_back(z.max_speed);
    }
}

ResonanceZones::ZonesVec ResonanceZones::sanitize_zones(const ZonesVec& zones) const
{
    ZonesVec clean;

    for (const auto& zone : zones) {
        // Skip zones that fail basic validation (min >= max, negative values)
        if (!zone.is_valid()) {
            continue;
        }

        // Skip if this zone overlaps or duplicates any already-accepted zone
        bool has_conflict = false;
        for (const auto& existing : clean) {
            // Check for duplicates (using zone's operator==, which uses epsilon)
            if (zone == existing) {
                has_conflict = true;
                break;
            }
            // Check for overlaps
            if (zone.overlaps_with(existing)) {
                has_conflict = true;
                break;
            }
        }

        if (!has_conflict) {
            clean.push_back(zone);
        }
    }

    return clean;
}

void ResonanceZones::sort_and_save_zones()
{
    if (!m_zones_config) {
        return;
    }

    auto zones = get_zones_from_config();

    // Sort zones by min_speed
    std::sort(zones.begin(), zones.end(), [](const ResonanceZone& a, const ResonanceZone& b) {
        return a.min_speed < b.min_speed;
    });

    save_zones(zones);

    // Rebuild UI to show sorted order
    rebuild_all_rows();
}

bool ResonanceZones::validate_zone(const ResonanceZone& zone, std::string& error_msg, const ResonanceZone& exclude) const
{
    if (!zone.is_valid(&error_msg)) {
        return false;
    }
    // Overlap checks skipped per requirements; only exclude equality checks
    if (zone == exclude) {
        return true;
    }
    return true;
}

bool ResonanceZones::check_overlap(const ResonanceZone& new_zone, const ResonanceZone& exclude) const
{
    if (!m_zones_config) {
        return false;
    }
    auto zones = get_zones_from_config();
    for (const auto& existing : zones) {
        if (existing == exclude) {
            continue;
        }
        if (new_zone.overlaps_with(existing)) {
            return true;
        }
    }
    return false;
}

const ResonanceZone* ResonanceZones::find_next_zone_after(double max_speed, const ZonesVec& zones) const
{
    const ResonanceZone* next_zone = nullptr;
    double min_gap = std::numeric_limits<double>::max();

    for (const auto& zone : zones) {
        // Use >= to catch adjacent zones (e.g., 85-90 followed by 90-100)
        if (zone.min_speed >= max_speed) {
            double gap = zone.min_speed - max_speed;
            if (gap < min_gap) {
                min_gap = gap;
                next_zone = &zone;
            }
        }
    }
    return next_zone;
}

void ResonanceZones::calculate_zone_from_gap(double gap, double& buffer, double& width) const
{
    if (gap >= 20) {
        buffer = 10;
        width = 10;
    } else if (gap >= 10) {
        buffer = 5;
        width = 5;
    } else if (gap >= 5) {
        buffer = 2;
        width = 3;
    } else if (gap >= 3) {
        buffer = 1;
        width = 2;
    } else {
        buffer = 0;
        width = 0;
    }
}

void ResonanceZones::add_zone_after(size_t zone_index)
{
    if (!m_zones_config || !is_valid_zone_index(zone_index)) {
        return;
    }

    auto zones = get_zones_from_config();

    if (MAX_RESONANCE_ZONES > 0 && zones.size() >= MAX_RESONANCE_ZONES) {
        wxMessageBox(wxString::Format(_L("Maximum %d zones allowed"), MAX_RESONANCE_ZONES),
                     _L("Cannot add zone"), wxOK | wxICON_WARNING);
        return;
    }

    const ResonanceZone& current_zone = zones[zone_index];

    // Find next zone after current
    auto next_zone = find_next_zone_after(current_zone.max_speed, zones);

    double buffer, width;

    if (next_zone != nullptr) {
        // Calculate gap to next zone
        double gap = next_zone->min_speed - current_zone.max_speed;

        // Get buffer and width based on gap
        calculate_zone_from_gap(gap, buffer, width);

        if (buffer == 0 || width == 0) {
            wxMessageBox(
                wxString::Format(_L("Not enough space between zones to add a new zone.\nAvailable gap: %.1f mm/s"), gap),
                _L("Cannot add zone"), wxOK | wxICON_WARNING);
            return;
        }
    } else {
        // Last zone - use default spacing
        buffer = 10;
        width = 10;
    }

    // Create new zone
    double new_min = current_zone.max_speed + buffer;
    double new_max = new_min + width;
    ResonanceZone new_zone(new_min, new_max);

    // Insert after current zone (using index, not search)
    zones.insert(zones.begin() + zone_index + 1, new_zone);

    // Don't sort here - only sort on load/save to avoid rows jumping during editing
    save_zones(zones);

    // Set flag to highlight the newly added zone after rebuild
    m_pending_highlight_zone = new_zone;
    m_pending_autoscroll = true;

    if (m_on_change_callback) {
        m_on_change_callback();
    }

    // Defer rebuild to avoid destroying widgets during button click event
    defer_rebuild();
}

void ResonanceZones::del_zone(const ResonanceZone& zone)
{
    if (!m_zones_config) {
        return;
    }

    auto zones = get_zones_from_config();

    if (zones.size() <= 1) {
        // Last zone - clear config and notify
        m_zones_config->values.clear();

        // Defer UI changes to avoid deleting recently-clicked button
        CallAfter([this]() {
            Hide();
            if (m_on_empty_callback) {
                m_on_empty_callback();
            }
        });
        return;
    }

    // Remove zone and rebuild
    ZonesVec new_zones = zones;
    auto it = std::find(new_zones.begin(), new_zones.end(), zone);
    if (it != new_zones.end()) {
        new_zones.erase(it);
    }
    save_zones(new_zones);

    if (m_on_change_callback) {
        m_on_change_callback();
    }

    // Defer rebuild to avoid destroying widgets during button click event
    defer_rebuild();
}

bool ResonanceZones::edit_zone(const ResonanceZone& old_zone, const ResonanceZone& new_zone)
{
    if (!m_zones_config) {
        return false;
    }

    // Allow any edits - validation happens only on save
    auto zones = get_zones_from_config();
    auto it = std::find(zones.begin(), zones.end(), old_zone);
    if (it != zones.end()) {
        *it = new_zone;
    }
    save_zones(zones);

    if (m_on_change_callback) {
        m_on_change_callback();
    }

    // Update values directly - no CallAfter needed since we're not destroying widgets
    update_zone_values();

    return true;
}

std::vector<ResonanceZones::ZoneValidationError> ResonanceZones::validate_all_zones() const
{
    const double EPSILON = 0.0001;  // Floating point comparison tolerance
    std::vector<ZoneValidationError> errors;
    auto zones = get_zones_from_config();

    for (size_t i = 0; i < zones.size(); ++i) {
        const auto& zone = zones[i];

        // Check 1: Invalid range (min >= max with epsilon tolerance)
        if (zone.max_speed - zone.min_speed < EPSILON) {
            errors.push_back({i, "Min speed must be less than max speed"});
        }

        // Check 2: Negative/zero values
        if (zone.min_speed <= 0) {
            errors.push_back({i, "Min speed must be positive"});
        }
        if (zone.max_speed <= 0) {
            errors.push_back({i, "Max speed must be positive"});
        }

        // Check 3: Duplicates and overlaps
        for (size_t j = i + 1; j < zones.size(); ++j) {
            const auto& other = zones[j];

            // Duplicate check
            if (std::abs(zone.min_speed - other.min_speed) < EPSILON &&
                std::abs(zone.max_speed - other.max_speed) < EPSILON) {
                errors.push_back({i, wxString::Format("Duplicate of row %zu", j + 1).ToStdString()});
            }
            // Overlap check (strict - no overlaps allowed)
            else if (zone.overlaps_with(other)) {
                errors.push_back({i, wxString::Format("Overlaps with row %zu", j + 1).ToStdString()});
            }
        }
    }

    return errors;
}

void ResonanceZones::update_parent_layout()
{
    if (!GetParent()) {
        return;
    }

    auto desired_size = GetBestSize();
    GetParent()->Layout();

    // Force size back if parent Layout() shrunk us below BestSize
    if (GetSize().GetHeight() < desired_size.GetHeight()) {
        SetSize(desired_size);
        InvalidateBestSize();
    }

    // Force parent to re-layout with our correct size
    if (GetParent()->GetParent()) {
        GetParent()->GetParent()->Layout();
    }

    GetParent()->Refresh();
}

void ResonanceZones::perform_autoscroll_to_bottom()
{
    auto* scroll_win = dynamic_cast<wxScrolledWindow*>(GetParent());
    if (!scroll_win) {
        return;
    }

    int unit_x, unit_y;
    scroll_win->GetScrollPixelsPerUnit(&unit_x, &unit_y);

    if (unit_y > 0) {
        // Get the virtual size (total scrollable content)
        wxSize virtual_size = scroll_win->GetVirtualSize();
        wxSize client_size = scroll_win->GetClientSize();

        // Calculate scroll position to show the bottom of content
        // Add a small margin to ensure the last row is fully visible
        int max_scroll_y = (virtual_size.GetHeight() - client_size.GetHeight() + unit_y) / unit_y;

        // Scroll to bottom to show newly added content
        if (max_scroll_y > 0) {
            scroll_win->Scroll(-1, max_scroll_y);
        }
    }
}

void ResonanceZones::highlight_zone_row(size_t zone_index)
{
    if (!is_valid_zone_index(zone_index)) {
        return;
    }

    auto& row = m_zone_rows[zone_index];

    // Focus on the min editor so user can type immediately
    if (row.min_editor && row.min_editor->GetTextCtrl()) {
        row.min_editor->GetTextCtrl()->SetFocus();
        row.min_editor->GetTextCtrl()->SelectAll();
    }
}

void ResonanceZones::set_extruder(int extruder_idx, ConfigOptionFloats* zones, bool visible)
{
    m_extruder_idx = extruder_idx;
    m_zones_config = zones;

    // Verify lifetime contract: zones pointer must be valid when widget is visible
    assert(!visible || zones != nullptr);

    if (visible) {
        // Only create a default zone if config is empty and we're becoming visible
        if (m_zones_config && m_zones_config->values.empty()) {
            m_zones_config->values = {DEFAULT_INITIAL_MIN_SPEED, DEFAULT_INITIAL_MAX_SPEED};
        }

        auto config_zones = get_zones_from_config();

        // Smart logic: check if zone count changed or if we're initializing
        if (config_zones.size() == m_zone_rows.size() && m_zone_rows.size() > 0) {
            // Same count and widgets exist - update values directly
            update_zone_values();

            // Show and layout after updating values
            Show();
            Layout();
            if (GetParent()) {
                GetParent()->Layout();
                GetParent()->Refresh();
            }
        } else {
            // Count changed or initializing - rebuild required
            if (!m_reload_pending) {
                m_reload_pending = true;

                // Always defer rebuild to avoid multiple rebuilds from rapid calls
                CallAfter([this]() {
                    m_reload_pending = false;

                    rebuild_all_rows();
                    Show();
                    Layout();

                    update_parent_layout();

                    // Highlight newly added zone if pending
                    if (m_pending_highlight_zone.has_value()) {
                        // Find the zone in the rebuilt rows
                        auto zones = get_zones_from_config();
                        for (size_t i = 0; i < zones.size(); ++i) {
                            if (std::abs(zones[i].min_speed - m_pending_highlight_zone->min_speed) < 0.01 &&
                                std::abs(zones[i].max_speed - m_pending_highlight_zone->max_speed) < 0.01) {
                                highlight_zone_row(i);
                                break;
                            }
                        }
                        m_pending_highlight_zone.reset();
                    }

                    // Only auto-scroll if user explicitly added a zone (clicked +)
                    if (m_pending_autoscroll) {
                        m_pending_autoscroll = false;
                        perform_autoscroll_to_bottom();
                    }
                });
            }
        }
    } else {
        // Clear zones when disabled
        if (m_zones_config) {
            m_zones_config->values.clear();
        }
        clear_ui();

        // Reset size so no white space is allocated
        SetMinSize(wxSize(-1, -1));
        SetSize(wxSize(-1, 0));
        InvalidateBestSize();

        Hide();
        if (GetParent()) {
            GetParent()->Layout();
            GetParent()->Refresh();
        }
    }
}

wxSize ResonanceZones::DoGetBestSize() const
{
    // Calculate size based on grid sizer's minimum size
    if (m_grid_sizer && m_zone_rows.size() > 0) {
        wxSize grid_size = m_grid_sizer->GetMinSize();
        return grid_size;
    }

    // Fallback to default
    return wxPanel::DoGetBestSize();
}

void ResonanceZones::msw_rescale()
{
    m_bmp_delete.msw_rescale();
    m_bmp_add.msw_rescale();
}

void ResonanceZones::sys_color_changed()
{
    // Recreate bitmaps with dark mode variants if needed
    const std::string delete_icon = wxGetApp().dark_mode() ? "delete_filament_dark" : "delete_filament";
    const std::string add_icon = wxGetApp().dark_mode() ? "add_filament_dark" : "add_filament";
    m_bmp_delete = ScalableBitmap(m_parent, delete_icon);
    m_bmp_add    = ScalableBitmap(m_parent, add_icon);

    reload_from_config();  // Rebuild UI with updated bitmaps for dark mode
}

}} // namespace Slic3r::GUI
