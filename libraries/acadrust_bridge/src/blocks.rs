// SPDX-License-Identifier: GPL-2.0-or-later

use super::{simple_block, validate, BridgeResult, Geometry, Shape};
use acadrust::tables::BlockRecord;
use acadrust::types::Vector3;
use acadrust::{CadDocument, Handle};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Serialize, PartialEq)]
pub(crate) struct Definition {
    base: [f64; 3],
    members: BTreeMap<u64, Geometry>,
}

#[derive(Deserialize)]
pub(crate) struct Edit {
    name: String,
    base: [f64; 3],
    create: bool,
    members: Vec<Member>,
}

#[derive(Deserialize)]
struct Member {
    handle: Option<u64>,
    geometry: Geometry,
}

fn basic(geometry: &Geometry) -> bool {
    !matches!(geometry.shape, Shape::Insert { .. })
}

pub(crate) fn definitions(doc: &CadDocument) -> BTreeMap<String, Definition> {
    doc.block_records.iter().filter_map(|block| {
        if !simple_block(doc, &block.name) {
            return None;
        }
        let mut members = BTreeMap::new();
        for handle in &block.entity_handles {
            let entity = doc.get_entity(*handle)?;
            let common = entity.common();
            if !common.reactors.is_empty() || !common.extended_data.is_empty() ||
                common.xdictionary_handle.is_some_and(|h| !h.is_null()) || common.has_ds_data {
                return None;
            }
            let geometry = Geometry::from_entity(doc, entity)?;
            if !basic(&geometry) {
                return None;
            }
            members.insert(handle.value(), geometry);
        }
        let base = block.base_point;
        Some((block.name.clone(), Definition { base: [base.x, base.y, base.z], members }))
    }).collect()
}

pub(crate) fn apply(doc: &mut CadDocument, edits: Vec<Edit>) -> BridgeResult<()> {
    if edits.is_empty() {
        return Ok(());
    }
    let baseline = definitions(doc);
    let mut names = BTreeSet::new();
    for edit in edits {
        let Edit { name, base, create, members } = edit;
        if name.trim() != name || name.is_empty() || name.len() > 255 ||
            name.chars().any(|c| c.is_control() || "<>/\\\":;?*|=,".contains(c)) ||
            !base.iter().all(|coordinate| coordinate.is_finite()) ||
            !names.insert(name.to_uppercase()) {
            return Err("invalid or duplicate block definition".into());
        }
        let [x, y, z] = base;
        let old = baseline.get(&name);
        let owner = if create {
            let mut record = BlockRecord::new(&name);
            record.handle = doc.allocate_handle();
            record.block_entity_handle = doc.allocate_handle();
            record.block_end_handle = doc.allocate_handle();
            record.base_point = Vector3::new(x, y, z);
            let owner = record.handle;
            doc.block_records.add(record)?;
            owner
        } else {
            if old.is_none() {
                return Err("block definition cannot be edited".into());
            }
            let record = doc.block_records.get_mut(&name).ok_or("missing block definition")?;
            record.base_point = Vector3::new(x, y, z);
            record.handle
        };
        let mut seen = BTreeSet::new();
        let mut order = Vec::new();
        for member in members {
            let geometry = member.geometry;
            if !basic(&geometry) {
                return Err("unsupported member in block definition".into());
            }
            validate(doc, &geometry)?;
            let handle = if let Some(id) = member.handle {
                if create || !old.is_some_and(|d| d.members.contains_key(&id)) || !seen.insert(id) {
                    return Err("invalid block member handle".into());
                }
                let handle = Handle::new(id);
                if old.and_then(|d| d.members.get(&id)) != Some(&geometry) {
                    geometry.apply(doc.get_entity_mut(handle).ok_or("missing block member")?)?;
                }
                handle
            } else {
                let mut entity = geometry.create();
                entity.common_mut().owner_handle = owner;
                doc.add_entity(entity)?
            };
            order.push(handle);
        }
        if !create {
            for id in old.unwrap().members.keys().filter(|id| !seen.contains(id)) {
                doc.remove_entity(Handle::new(*id)).ok_or("missing deleted block member")?;
            }
        }
        doc.block_records.get_mut(&name).ok_or("missing block definition")?.entity_handles = order;
    }
    Ok(())
}
