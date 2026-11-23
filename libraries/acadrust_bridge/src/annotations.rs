// SPDX-License-Identifier: GPL-2.0-or-later

use super::BridgeResult;
use acadrust::entities::{Dimension, DimensionBase, DrawingDirection};
use acadrust::io::dxf::DxfReader;
use acadrust::tables::BlockRecord;
use acadrust::types::Vector3;
use acadrust::{CadDocument, EntityType, Handle};
use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

#[derive(Deserialize)]
pub(crate) struct Patch {
    path: PathBuf,
    edits: Vec<Edit>,
}

#[derive(Deserialize)]
struct Edit {
    handle: Option<u64>,
    kind: String,
    preserve_style: bool,
    wrap_width: Option<f64>,
}

fn dimension(entity: &EntityType) -> Option<&DimensionBase> {
    match entity {
        EntityType::Dimension(d) if kind_is_dimension(d) => Some(d.base()),
        _ => None,
    }
}

fn dimension_mut(entity: &mut EntityType) -> Option<&mut DimensionBase> {
    match entity {
        EntityType::Dimension(d) if kind_is_dimension(d) => Some(d.base_mut()),
        _ => None,
    }
}

fn kind_is_dimension(d: &Dimension) -> bool {
    matches!(d, Dimension::Linear(_) | Dimension::Angular2Ln(_))
}

fn kind(entity: &EntityType) -> Option<&'static str> {
    match entity {
        EntityType::Hatch(_) => Some("hatch"),
        EntityType::Text(_) => Some("text"),
        EntityType::MText(_) => Some("mtext"),
        EntityType::Dimension(d) => match d.as_ref() {
            Dimension::Linear(_) => Some("dim_linear"),
            Dimension::Angular2Ln(_) => Some("dim_angular"),
            _ => None,
        },
        _ => None,
    }
}

fn finish_hatch(hatch: &mut acadrust::entities::Hatch) -> BridgeResult<()> {
    use acadrust::entities::HatchPatternLine;
    use acadrust::types::Vector2;
    use acadrust::xdata::XDataValue;

    if !simple_hatch(hatch) {
        return Err("unsupported hatch boundary or style".into());
    }
    if hatch.is_solid {
        return Ok(());
    }
    if !hatch.pattern_angle.is_finite() || hatch.pattern_scale <= 0.0 ||
        !hatch.pattern_scale.is_finite() {
        return Err("invalid hatch angle or scale".into());
    }
    let marker = hatch.common.extended_data.get_record("LibreCad").is_some_and(|record|
        record.values.iter().any(|value|
            matches!(value, XDataValue::String(text) if text == "HATCH_LINES_APPROXIMATE")));
    if marker {
        // Families match the bundled tiles. ANSI31 intercepts advance by 2
        // units along a tile axis, and SQUARE repeats a 50-unit grid.
        let families: &[(f64, f64)] = match hatch.pattern.name.to_ascii_uppercase().as_str() {
            "ANSI31" => &[(std::f64::consts::FRAC_PI_4, std::f64::consts::SQRT_2)],
            "SQUARE" => &[(0.0, 50.0), (std::f64::consts::FRAC_PI_2, 50.0)],
            _ => return Err("this hatch pattern has no exact DWG definition; use ANSI31, SQUARE or a solid fill".into()),
        };
        hatch.pattern.lines = families.iter().map(|(direction, pitch)| {
            let angle = (hatch.pattern_angle + direction).rem_euclid(std::f64::consts::TAU);
            let spacing = pitch * hatch.pattern_scale;
            HatchPatternLine {
                angle,
                base_point: Vector2::ZERO,
                offset: Vector2::new(-angle.sin() * spacing, angle.cos() * spacing),
                dash_lengths: Vec::new(),
            }
        }).collect();
        if let Some(mut record) = hatch.common.extended_data.remove_record("LibreCad") {
            record.values.retain(|value|
                !matches!(value, XDataValue::String(text) if text == "HATCH_LINES_APPROXIMATE"));
            if !record.values.is_empty() {
                hatch.common.extended_data.add_record(record);
            }
        }
    }
    if hatch.pattern.lines.is_empty() {
        return Err("missing hatch pattern lines".into());
    }
    Ok(())
}

fn simple_hatch(hatch: &acadrust::entities::Hatch) -> bool {
    use acadrust::entities::{BoundaryEdge, HatchStyleType};
    !hatch.is_mpolygon && !hatch.is_associative && !hatch.gradient_color.enabled &&
        hatch.elevation == 0.0 && hatch.normal == Vector3::UNIT_Z &&
        hatch.style == HatchStyleType::Normal && !hatch.is_double &&
        !hatch.paths.is_empty() && hatch.paths.iter().all(|path|
            path.boundary_handles.is_empty() && !path.edges.is_empty() &&
            path.edges.iter().all(|edge| matches!(edge,
                BoundaryEdge::Line(_) | BoundaryEdge::CircularArc(_) | BoundaryEdge::Polyline(_))))
}

fn editable(entity: &EntityType) -> bool {
    let common = entity.common();
    if common.xdictionary_handle.is_some_and(|handle| !handle.is_null()) ||
        !common.reactors.is_empty() || !common.extended_data.is_empty() || common.has_ds_data {
        return false;
    }
    match entity {
        EntityType::Hatch(hatch) => simple_hatch(hatch),
        EntityType::Text(t) => t.insertion_point.z == 0.0 &&
            t.alignment_point.is_none_or(|p| p.z == 0.0) && t.thickness == 0.0 &&
            t.oblique_angle == 0.0 && t.generation_flags == 0 && t.normal == Vector3::UNIT_Z &&
            !t.value.contains("%<"),
        EntityType::MText(t) => t.insertion_point.z == 0.0 && t.normal == Vector3::UNIT_Z &&
            !t.is_annotative && t.column_data.column_type == 0 && t.background_fill_flags == 0 &&
            t.drawing_direction != DrawingDirection::TopToBottom && !t.value.contains("%<"),
        _ => dimension(entity).is_some_and(|d| d.normal == Vector3::UNIT_Z &&
            d.definition_point.z == 0.0 && d.text_middle_point.z == 0.0 &&
            !d.user_text.as_ref().is_some_and(|text| text.contains("%<"))),
    }
}

pub(crate) fn model(doc: &CadDocument) -> BTreeMap<u64, &'static str> {
    doc.model_space_entities().filter(|entity| editable(entity))
        .filter_map(|entity| kind(entity).map(|name| (entity.common().handle.value(), name)))
        .collect()
}

fn style(entity: &EntityType) -> &str {
    match entity {
        EntityType::Text(t) => &t.style,
        EntityType::MText(t) => &t.style,
        _ => &dimension(entity).expect("checked annotation type").style_name,
    }
}

fn set_style(entity: &mut EntityType, value: String) {
    match entity {
        EntityType::Text(t) => t.style = value,
        EntityType::MText(t) => t.style = value,
        _ => dimension_mut(entity).expect("checked annotation type").style_name = value,
    }
}

fn add_text_style(doc: &mut CadDocument, patch: &CadDocument, name: &str) -> BridgeResult<()> {
    if !doc.text_styles.contains(name) {
        let mut style = patch.text_styles.get(name).ok_or("missing annotation text style")?.clone();
        style.handle = doc.allocate_handle();
        doc.text_styles.add(style)?;
    }
    Ok(())
}

fn dimension_block(doc: &mut CadDocument, patch: &CadDocument, source: &str) -> BridgeResult<String> {
    let record = patch.block_records.get(source).ok_or("missing regenerated dimension block")?;
    let name = (1..).map(|i| format!("*D{i}"))
        .find(|name| !doc.block_records.contains(name)).ok_or("no available dimension block name")?;
    let mut block = BlockRecord::new(&name);
    block.handle = doc.allocate_handle();
    block.block_entity_handle = doc.allocate_handle();
    block.block_end_handle = doc.allocate_handle();
    block.flags.anonymous = true;
    block.base_point = record.base_point;
    let owner = block.handle;
    doc.block_records.add(block)?;
    if record.entity_handles.is_empty() {
        return Err(format!("regenerated dimension block {source} is empty").into());
    }
    for source_entity in patch.entities_in_block(source) {
        let mut entity = source_entity.clone();
        match &entity {
            EntityType::Text(_) | EntityType::MText(_) => add_text_style(doc, patch, style(&entity))?,
            EntityType::Line(_) | EntityType::Arc(_) | EntityType::Solid(_) |
            EntityType::Circle(_) | EntityType::LwPolyline(_) | EntityType::Point(_) => {}
            _ => return Err("unsupported component in regenerated dimension block".into()),
        }
        let common = entity.common_mut();
        if !doc.layers.contains(&common.layer) || !doc.line_types.contains(&common.linetype) {
            return Err("missing dimension component layer or linetype".into());
        }
        common.handle = Handle::NULL;
        common.owner_handle = owner;
        common.layer_handle = None;
        common.linetype_handle = None;
        common.entity_mode = None;
        doc.add_entity(entity)?;
    }
    Ok(name)
}

impl Patch {
    pub(crate) fn apply(self, doc: &mut CadDocument, used: &mut BTreeSet<u64>) -> BridgeResult<()> {
        let original = model(doc);
        let draft = DxfReader::from_file(self.path)?.read()?;
        let entities: Vec<_> = draft.model_space_entities().filter(|e| kind(e).is_some()).collect();
        if entities.len() != self.edits.len() {
            return Err("wrong number of projected annotations".into());
        }
        for (edit, entity) in self.edits.into_iter().zip(entities) {
            let expected = edit.kind.as_str();
            if kind(entity) != Some(expected) {
                return Err("annotation projection type mismatch".into());
            }
            let previous = if let Some(handle) = edit.handle {
                if original.get(&handle) != Some(&expected) || !used.insert(handle) {
                    return Err(format!("invalid or repeated annotation handle {handle:X}").into());
                }
                Some(doc.get_entity(Handle::new(handle)).ok_or("annotation disappeared")?.clone())
            } else {
                None
            };
            let mut next = entity.clone();
            if let EntityType::MText(text) = &mut next {
                let width = edit.wrap_width.ok_or("missing MTEXT wrap width")?;
                if !width.is_finite() || width < 0.0 {
                    return Err("invalid MTEXT wrap width".into());
                }
                text.rectangle_width = width;
            }
            if let EntityType::Hatch(hatch) = &mut next {
                finish_hatch(hatch)?;
            } else {
                let desired_style = if edit.preserve_style {
                    style(previous.as_ref().ok_or("missing original annotation style")?)
                } else {
                    style(entity)
                };
                set_style(&mut next, desired_style.to_owned());
                if let EntityType::Dimension(dim) = &mut next {
                    let measurement = dim.stored_measurement();
                    dim.base_mut().actual_measurement = measurement;
                }
                if let Some(base) = dimension_mut(&mut next) {
                    if !doc.dim_styles.contains(&base.style_name) {
                        return Err(format!("unknown dimension style {}", base.style_name).into());
                    }
                    if let Some(old) = previous.as_ref().and_then(dimension) {
                        base.version = old.version;
                    }
                    base.block_name = dimension_block(doc, &draft, &base.block_name)?;
                } else {
                    add_text_style(doc, &draft, desired_style)?;
                }
            }
            let layer = entity.common().layer.clone();
            if !doc.layers.contains(&layer) {
                return Err("annotation layer does not exist in original DWG".into());
            }
            let common = next.common_mut();
            if let Some(old) = &previous {
                *common = old.common().clone();
                common.graphic_data = None;
            } else {
                common.handle = Handle::NULL;
                common.owner_handle = Handle::NULL;
                common.entity_mode = None;
                common.linetype_handle = None;
            }
            if previous.is_none() || common.layer != layer {
                common.layer = layer;
                common.layer_handle = None;
            }
            common.raw_record = None;
            if let Some(handle) = edit.handle {
                *doc.get_entity_mut(Handle::new(handle)).ok_or("annotation disappeared")? = next;
            } else {
                doc.add_entity(next)?;
            }
        }
        Ok(())
    }
}

pub(crate) fn blocks(doc: &CadDocument) -> BTreeMap<u64, (u64, Vec<u64>)> {
    doc.model_space_entities().filter_map(|entity| {
        let base = dimension(entity)?;
        let block = doc.block_records.get(&base.block_name)?;
        Some((entity.common().handle.value(), (block.handle.value(),
            block.entity_handles.iter().map(|h| h.value()).collect())))
    }).collect()
}
