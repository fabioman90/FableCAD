// SPDX-License-Identifier: GPL-2.0-or-later

mod annotations;
mod blocks;
mod layers;

use acadrust::{Arc, CadDocument, Circle, DwgReader, DwgWriter, DxfVersion, EntityType, Handle, Line, LwPolyline};
use acadrust::entities::{Insert, LwVertex};
use acadrust::types::{Vector2, Vector3};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::io::{self, BufRead, Write};
use std::path::{Path, PathBuf};

type BridgeResult<T> = Result<T, Box<dyn std::error::Error>>;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
struct Geometry {
    layer: String,
    #[serde(flatten)]
    shape: Shape,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(tag = "shape", rename_all = "snake_case")]
enum Shape {
    Line { start: [f64; 3], end: [f64; 3] },
    Circle { center: [f64; 2], radius: f64 },
    Arc { center: [f64; 2], radius: f64, start_angle: f64, end_angle: f64 },
    Polyline { vertices: Vec<[f64; 3]>, closed: bool },
    Insert { block: String, position: [f64; 3], scale: [f64; 3], rotation: f64 },
}

impl Geometry {
    fn from_entity(doc: &CadDocument, entity: &EntityType) -> Option<Self> {
        let shape = match entity {
            EntityType::Line(line) => Shape::Line {
                start: [line.start.x, line.start.y, line.start.z],
                end: [line.end.x, line.end.y, line.end.z],
            },
            EntityType::Circle(circle) if circle.center.z == 0.0 &&
                circle.thickness == 0.0 && circle.normal == Vector3::UNIT_Z => Shape::Circle {
                center: [circle.center.x, circle.center.y], radius: circle.radius,
            },
            EntityType::Arc(arc) if arc.center.z == 0.0 &&
                arc.thickness == 0.0 && arc.normal == Vector3::UNIT_Z => Shape::Arc {
                center: [arc.center.x, arc.center.y], radius: arc.radius,
                start_angle: arc.start_angle, end_angle: arc.end_angle,
            },
            EntityType::LwPolyline(polyline) if polyline.vertices.len() >= 2 &&
                polyline.elevation == 0.0 && polyline.thickness == 0.0 &&
                polyline.constant_width == 0.0 && polyline.normal == Vector3::UNIT_Z &&
                polyline.vertices.iter().all(|v| v.start_width == 0.0 &&
                    v.end_width == 0.0 && v.vertex_id == 0) => {
                let mut vertices: Vec<_> = polyline.vertices.iter()
                    .map(|v| [v.location.x, v.location.y, v.bulge]).collect();
                if !polyline.is_closed {
                    vertices.last_mut().unwrap()[2] = 0.0;
                }
                Shape::Polyline { vertices, closed: polyline.is_closed }
            }
            EntityType::Insert(insert) if simple_insert(doc, insert) => Shape::Insert {
                block: insert.block_name.clone(),
                position: [insert.insert_point.x, insert.insert_point.y, insert.insert_point.z],
                scale: [insert.x_scale(), insert.y_scale(), insert.z_scale()],
                rotation: insert.rotation,
            },
            _ => return None,
        };
        Some(Self { layer: entity.common().layer.clone(), shape })
    }

    fn create(&self) -> EntityType {
        let mut entity = match &self.shape {
            Shape::Line { start: [x, y, z], end: [u, v, w] } =>
                EntityType::Line(Line::from_coords(*x, *y, *z, *u, *v, *w)),
            Shape::Circle { center: [x, y], radius } =>
                EntityType::Circle(Circle::from_center_radius(Vector3::new(*x, *y, 0.0), *radius)),
            Shape::Arc { center: [x, y], radius, start_angle, end_angle } =>
                EntityType::Arc(Arc::from_coords(*x, *y, 0.0, *radius, *start_angle, *end_angle)),
            Shape::Insert { block, position: [x, y, z], scale: [sx, sy, sz], rotation } =>
                EntityType::Insert(Insert::new(block, Vector3::new(*x, *y, *z))
                    .with_scale(*sx, *sy, *sz).with_rotation(*rotation)),
            Shape::Polyline { vertices, closed } => {
                let mut polyline = LwPolyline::new();
                polyline.vertices = vertices.iter().map(|[x, y, bulge]|
                    LwVertex::with_bulge(Vector2::new(*x, *y), *bulge)).collect();
                polyline.is_closed = *closed;
                EntityType::LwPolyline(polyline)
            }
        };
        entity.common_mut().layer.clone_from(&self.layer);
        entity
    }

    fn apply(&self, current: &mut EntityType) -> BridgeResult<()> {
        let mut replacement = self.create();
        match (&*current, &mut replacement) {
            (EntityType::Line(old), EntityType::Line(new)) => {
                new.thickness = old.thickness;
                new.normal = old.normal;
            }
            (EntityType::Insert(old), EntityType::Insert(new)) if old.block_name == new.block_name => {}
            (EntityType::Circle(_), EntityType::Circle(_)) |
            (EntityType::Arc(_), EntityType::Arc(_)) => {}
            (EntityType::LwPolyline(old), EntityType::LwPolyline(new)) => {
                new.plinegen = old.plinegen;
                if !old.is_closed && !new.is_closed && old.vertices.len() == new.vertices.len() {
                    let old_last = old.vertices.last().unwrap();
                    new.vertices.last_mut().unwrap().bulge = old_last.bulge;
                }
            }
            _ => return Err("DWG entity type changed".into()),
        }
        let common = replacement.common_mut();
        let same_layer = current.common().layer == self.layer;
        let new_layer_handle = common.layer_handle;
        *common = current.common().clone();
        if !same_layer {
            common.layer.clone_from(&self.layer);
            common.layer_handle = new_layer_handle;
        }
        common.raw_record = None;
        if matches!(self.shape, Shape::Insert { .. }) {
            common.graphic_data = None;
        }
        *current = replacement;
        Ok(())
    }
}

#[derive(Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
enum Operation {
    Add { geometry: Geometry },
    Update { handle: u64, geometry: Geometry },
    Delete { handle: u64 },
}

#[derive(Deserialize)]
#[serde(tag = "command", rename_all = "snake_case", deny_unknown_fields)]
enum Request {
    Open { path: PathBuf, projection: Option<PathBuf> },
    Apply { operations: Vec<Operation> },
    Undo,
    Redo,
    Save { path: PathBuf },
    SaveEdited { path: PathBuf, operations: Vec<Operation>, annotations: Option<annotations::Patch>,
                 #[serde(default)] layer_edits: Vec<layers::Edit>,
                 #[serde(default)] block_edits: Vec<blocks::Edit>,
                 #[serde(default)] original_format: bool },
}

#[derive(Default)]
struct Session {
    current: Option<CadDocument>,
    undo: Vec<CadDocument>,
    redo: Vec<CadDocument>,
}

impl Session {
    fn document(&self) -> BridgeResult<&CadDocument> {
        self.current.as_ref().ok_or_else(|| "no DWG is open".into())
    }

    fn open(&mut self, path: &Path, projection: Option<&Path>) -> BridgeResult<Value> {
        let doc = DwgReader::from_file(path)?.read()?;
        if let Some(projection) = projection {
            write_new(projection, &doc, None)?;
        }
        let entities = model_geometry(&doc);
        self.current = Some(doc);
        self.undo.clear();
        self.redo.clear();
        Ok(json!({ "entities": entities, "annotations": annotations::model(self.document()?), "blocks": blocks::definitions(self.document()?), "source_version": self.document()?.version.as_str() }))
    }

    fn edited(&self, operations: Vec<Operation>, annotations: Option<annotations::Patch>, layer_edits: Vec<layers::Edit>, block_edits: Vec<blocks::Edit>) -> BridgeResult<(CadDocument, Vec<u64>)> {
        let mut next = self.document()?.clone();
        layers::apply(&mut next, layer_edits)?;
        blocks::apply(&mut next, block_edits)?;
        let model_handles: BTreeSet<_> = model_geometry(&next).keys().copied().collect();
        let annotation_handles = annotations::model(&next);
        let mut added = Vec::new();
        let mut edited = BTreeSet::new();
        for operation in operations {
            match operation {
                Operation::Add { geometry } => {
                    validate(&next, &geometry)?;
                    added.push(next.add_entity(geometry.create())?.value());
                }
                Operation::Update { handle, geometry } => {
                    validate(&next, &geometry)?;
                    if !model_handles.contains(&handle) || !edited.insert(handle) {
                        return Err(format!("invalid or repeated model-space entity handle {handle:X}").into());
                    }
                    let current = next.get_entity_mut(Handle::new(handle))
                        .ok_or("DWG entity is missing")?;
                    geometry.apply(current)?;
                }
                Operation::Delete { handle } => {
                    if !(model_handles.contains(&handle) || annotation_handles.contains_key(&handle)) || !edited.insert(handle) {
                        return Err(format!("invalid or repeated model-space entity handle {handle:X}").into());
                    }
                    next.remove_entity(Handle::new(handle)).ok_or("entity disappeared before deletion")?;
                }
            }
        }
        if let Some(patch) = annotations {
            patch.apply(&mut next, &mut edited)?;
        }
        Ok((next, added))
    }

    fn apply(&mut self, operations: Vec<Operation>) -> BridgeResult<Value> {
        let (next, added) = self.edited(operations, None, Vec::new(), Vec::new())?;
        if let Some(previous) = self.current.replace(next) {
            self.undo.push(previous);
            self.redo.clear();
        }
        Ok(json!({ "added": added }))
    }

    fn run(&mut self, request: Request) -> BridgeResult<Value> {
        match request {
            Request::Open { path, projection } => self.open(&path, projection.as_deref()),
            Request::Apply { operations } => self.apply(operations),
            Request::Undo => {
                let current = self.document()?;
                if self.undo.is_empty() {
                    return Err("nothing to undo".into());
                }
                self.redo.push(current.clone());
                self.current = self.undo.pop();
                Ok(json!({}))
            }
            Request::Redo => {
                let current = self.document()?;
                if self.redo.is_empty() {
                    return Err("nothing to redo".into());
                }
                self.undo.push(current.clone());
                self.current = self.redo.pop();
                Ok(json!({}))
            }
            Request::SaveEdited { path, operations, annotations, layer_edits, block_edits, original_format } => {
                let (mut output, _) = self.edited(operations, annotations, layer_edits, block_edits)?;
                if !original_format {
                    output.version = DxfVersion::AC1032;
                }
                let opaque_source = if original_format { Some(self.document()?) } else { None };
                write_new(&path, &output, opaque_source)?;
                Ok(json!({ "path": path }))
            }
            Request::Save { path } => {
                let mut output = self.document()?.clone();
                output.version = DxfVersion::AC1032;
                write_new(&path, &output, None)?;
                Ok(json!({ "path": path }))
            }
        }
    }
}

fn validate(doc: &CadDocument, geometry: &Geometry) -> BridgeResult<()> {
    let finite = |values: &[f64]| values.iter().all(|n| n.is_finite());
    let valid = match &geometry.shape {
        Shape::Line { start, end } => finite(start) && finite(end),
        Shape::Circle { center, radius } => finite(center) && radius.is_finite() && *radius > 0.0,
        Shape::Arc { center, radius, start_angle, end_angle } =>
            finite(center) && finite(&[*radius, *start_angle, *end_angle]) && *radius > 0.0 &&
            (*end_angle - *start_angle).rem_euclid(std::f64::consts::TAU) != 0.0,
        Shape::Insert { block, position, scale, rotation } =>
            finite(position) && finite(scale) && rotation.is_finite() &&
            scale.iter().all(|factor| *factor != 0.0) && simple_block(doc, block),
        Shape::Polyline { vertices, closed } => vertices.len() >= 2 &&
            vertices.iter().all(|v| finite(v)) && (*closed || vertices.last().unwrap()[2] == 0.0),
    };
    if !valid {
        return Err("invalid DWG geometry".into());
    }
    if !doc.layers.contains(&geometry.layer) {
        return Err(format!("unknown layer {}", geometry.layer).into());
    }
    Ok(())
}

fn simple_block(doc: &CadDocument, name: &str) -> bool {
    doc.block_records.get(name).is_some_and(|block| !block.flags.anonymous && !block.flags.is_xref && !block.flags.is_external && !block.flags.is_xref_overlay &&
        !block.name.starts_with('*') && !block.name.contains('|') &&
        doc.dynamic_block_definition(block.handle).is_none() &&
        !doc.entities_in_block(name).any(|e| matches!(e, EntityType::AttributeDefinition(_))))
}

fn simple_insert(doc: &CadDocument, insert: &Insert) -> bool {
    let common = &insert.common;
    insert.normal == Vector3::UNIT_Z && insert.row_count == 1 && insert.column_count == 1 &&
        insert.row_spacing == 0.0 && insert.column_spacing == 0.0 &&
        insert.attributes.is_empty() && insert.view_rep_handle.is_none_or(|h| h.is_null()) &&
        common.reactors.is_empty() && common.xdictionary_handle.is_none_or(|h| h.is_null()) &&
        common.extended_data.is_empty() && !common.has_ds_data &&
        simple_block(doc, &insert.block_name)
}

fn model_geometry(doc: &CadDocument) -> BTreeMap<u64, Geometry> {
    doc.model_space_entities()
        .filter_map(|entity| Geometry::from_entity(doc, entity)
            .map(|geometry| (entity.common().handle.value(), geometry)))
        .collect()
}

fn verify_opaque_objects(expected: &CadDocument, actual: &CadDocument) -> BridgeResult<()> {
    use acadrust::objects::ObjectType;
    for (handle, object) in &expected.objects {
        if let ObjectType::Unknown { type_name, owner, raw_dwg_data, raw_dwg_handle_bits, .. } = object {
            let Some(ObjectType::Unknown {
                type_name: written_type, owner: written_owner, raw_dwg_data: written_data,
                raw_dwg_handle_bits: written_handle_bits, ..
            }) = actual.objects.get(handle) else {
                return Err(format!("DWG lost opaque {type_name} object {handle}").into());
            };
            if written_type != type_name || written_owner != owner ||
                written_data != raw_dwg_data || written_handle_bits != raw_dwg_handle_bits {
                return Err(format!("DWG changed opaque {type_name} object {handle}").into());
            }
        }
    }
    Ok(())
}

fn write_new(path: &Path, doc: &CadDocument, opaque_source: Option<&CadDocument>) -> BridgeResult<()> {
    let mut file = std::fs::OpenOptions::new().write(true).create_new(true).open(path)?;
    let result = (|| -> BridgeResult<()> {
        let bytes = DwgWriter::write_to_vec(doc)?;
        file.write_all(&bytes)?;
        file.sync_all()?;
        let reopened = DwgReader::from_file(path)?.read()?;
        if reopened.version != doc.version || model_geometry(&reopened) != model_geometry(doc) {
            return Err("DWG verification failed for model-space geometry".into());
        }
        if blocks::definitions(&reopened) != blocks::definitions(doc) {
            return Err("DWG verification failed for simple block definitions".into());
        }
        if let Some(source) = opaque_source {
            verify_opaque_objects(source, &reopened)?;
        }
        if layers::identity(&reopened) != layers::identity(doc) {
            return Err("DWG verification failed for layer names or handles".into());
        }
        if annotations::model(&reopened) != annotations::model(doc) {
            return Err("DWG verification failed for model-space annotations".into());
        }
        if annotations::blocks(&reopened) != annotations::blocks(doc) {
            return Err("DWG verification failed for dimension block references".into());
        }
        Ok(())
    })();
    drop(file);
    if result.is_err() {
        std::fs::remove_file(path)?;
    }
    result
}

fn main() -> io::Result<()> {
    let mut session = Session::default();
    let mut output = io::stdout().lock();
    for input in io::stdin().lock().lines() {
        let response = match serde_json::from_str::<Request>(&input?) {
            Ok(request) => match session.run(request) {
                Ok(result) => json!({"ok": true, "result": result}),
                Err(error) => json!({"ok": false, "error": error.to_string()}),
            },
            Err(error) => json!({"ok": false, "error": error.to_string()}),
        };
        writeln!(output, "{response}")?;
        output.flush()?;
    }
    Ok(())
}
