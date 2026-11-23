// SPDX-License-Identifier: GPL-2.0-or-later

use super::BridgeResult;
use acadrust::tables::Layer;
use acadrust::types::{Color, LineWeight};
use acadrust::CadDocument;
use serde::Deserialize;

#[derive(Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub(crate) enum Edit {
    Rename { old: String, name: String },
    Add {
        name: String,
        rgb: [u8; 3],
        width: i16,
        locked: bool,
        frozen: bool,
        off: bool,
        plot: bool,
    },
}

pub(crate) fn apply(doc: &mut CadDocument, edits: Vec<Edit>) -> BridgeResult<()> {
    for edit in edits {
        match edit {
            Edit::Rename { old, name } => {
                doc.rename_layer(&old, &name)?;
            }
            Edit::Add { name, rgb: [r, g, b], width, locked, frozen, off, plot } => {
                if name.trim() != name || name.is_empty() || name.len() > 255 ||
                    name.chars().any(|c| c.is_control() || "<>/\\\":;?*|=,".contains(c)) {
                    return Err("invalid DWG layer name".into());
                }
                let mut layer = Layer::with_color(name, Color::from_rgb(r, g, b));
                layer.handle = doc.allocate_handle();
                layer.line_weight = LineWeight::from_value(width);
                layer.flags.locked = locked;
                layer.flags.frozen = frozen;
                layer.flags.off = off;
                layer.is_plottable = plot;
                doc.layers.add(layer)?;
            }
        }
    }
    Ok(())
}

pub(crate) fn identity(doc: &CadDocument) -> std::collections::BTreeMap<&str, u64> {
    doc.layers.iter().map(|layer| (layer.name.as_str(), layer.handle.value())).collect()
}
