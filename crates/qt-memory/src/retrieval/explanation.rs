//! JSON explanations of recall results, typed retrieval candidates and context
//! previews (archived `retrieval_explanation.cpp`). Previews never claim that
//! context was injected or used.

use serde_json::{Value, json};

use super::{
    MemoryAssertion, RetrievalCandidate, RetrievalChunk, RetrievalOwnerKind, RetrievalReference,
};
use crate::context::ContextPack;
use crate::recall::RecalledMemory;
use crate::text::canonical;

fn reference_json(reference: &RetrievalReference) -> Value {
    json!({
        "kind": reference.kind.as_str(), "id": reference.id, "revision": reference.revision,
        "repository": reference.repository, "worktree": reference.worktree, "path": reference.path,
        "source_fingerprint": reference.source_fingerprint,
        "snapshot_generation": reference.snapshot_generation,
    })
}

pub(crate) fn chunk_json(chunk: &RetrievalChunk) -> Value {
    json!({
        "id": chunk.id, "owner_key": chunk.owner_key, "owner_revision": chunk.owner_revision,
        "ordinal": chunk.ordinal, "text": chunk.text, "content_hash": chunk.content_hash,
        "source_uri": chunk.source_uri, "source_span": chunk.source_span,
        "render_version": chunk.render_version,
    })
}

fn assertion_json(assertion: &MemoryAssertion) -> Value {
    let evidence: Vec<Value> = assertion
        .evidence
        .iter()
        .map(|item| {
            json!({
                "source_id": item.source_id, "source_revision": item.source_revision,
                "source_uri": item.source_uri, "source_hash": item.source_hash,
                "source_span": item.source_span, "created_by": item.created_by,
            })
        })
        .collect();
    json!({
        "memory_id": assertion.memory_id, "revision": assertion.revision,
        "subject_id": assertion.subject_id, "predicate": assertion.predicate,
        "object_id": assertion.object_id, "literal_json": assertion.literal_json,
        "qualifiers_json": assertion.qualifiers_json,
        "polarity": if assertion.positive { "positive" } else { "negative" },
        "positive": assertion.positive,
        "valid_from": assertion.valid_from.unwrap_or(0),
        "valid_to": assertion.valid_to.unwrap_or(0),
        "recorded_at": assertion.recorded_at,
        "evidence": evidence,
    })
}

fn selection_json(pack: &ContextPack, owner: &RetrievalReference, eligible: bool) -> Value {
    let selected = match owner.kind {
        RetrievalOwnerKind::Memory => Some(pack.memory_ids.contains(&owner.id)),
        RetrievalOwnerKind::Code => Some(pack.code_ids.contains(&owner.id)),
        RetrievalOwnerKind::Entity => None,
    };
    let mut indices = Vec::new();
    let mut reasons: Vec<String> = Vec::new();
    let mut has_validation = false;
    let mut any_eligible = false;
    for (index, item) in pack.item_explanations.iter().enumerate() {
        let ids = if owner.kind == RetrievalOwnerKind::Code {
            &item.code_ids
        } else {
            &item.memory_ids
        };
        if owner.kind == RetrievalOwnerKind::Entity || !ids.contains(&owner.id) {
            continue;
        }
        indices.push(index);
        has_validation = true;
        any_eligible |= item.eligible;
        if !item.selected && !reasons.contains(&item.reason) {
            reasons.push(item.reason.clone());
        }
    }
    let final_eligible = eligible && (!has_validation || any_eligible);
    let reason = if !final_eligible {
        "ineligible"
    } else {
        match selected {
            None => "entity metadata selection is bundle-owned",
            Some(true) => "selected",
            Some(false) => "not selected; see item reasons",
        }
    };
    json!({
        "preview": true, "retrieved": true, "eligible": final_eligible,
        "selected": selected, "injected": false, "used": null, "reason": reason,
        "item_indices": indices, "item_omission_reasons": reasons,
    })
}

/// One governed recall result. `current_revision` is supplied by the caller
/// after its visibility check; historical results keep their captured
/// revision.
pub fn recalled_memory_json(
    item: &RecalledMemory,
    current_revision: Option<i64>,
    preview: Option<&ContextPack>,
) -> Value {
    let memory = &item.memory;
    let graph_path: Vec<Value> = (0..item.graph_path_ids.len().saturating_sub(1))
        .map(|index| {
            let reverse = item.graph_path_reverse.get(index).copied().unwrap_or(false);
            let from = &item.graph_path_ids[index + usize::from(reverse)];
            let to = &item.graph_path_ids[index + usize::from(!reverse)];
            json!({
                "from": {"kind": "memory", "id": from},
                "to": {"kind": "memory", "id": to},
                "relation": item.graph_path_relations.get(index).cloned().unwrap_or_default(),
                "reverse": reverse,
                "provenance": "stored relation",
            })
        })
        .collect();
    let assertion_path: Vec<Value> = item
        .assertion_path
        .iter()
        .map(|step| {
            json!({
                "memory_id": step.memory_id, "revision": step.revision,
                "from_entity_id": step.from_entity_id, "to_entity_id": step.to_entity_id,
                "provenance": "stored assertion", "reverse": step.reverse,
            })
        })
        .collect();
    let basis = if item.historical {
        "historical_capture"
    } else if item.dense_match.is_some() {
        "dense_capture"
    } else {
        "current_canonical_at_explanation"
    };
    let number = if item.historical {
        json!(item.historical_revision)
    } else if let Some(hit) = &item.dense_match {
        json!(hit.chunk.owner_revision)
    } else {
        json!(current_revision)
    };
    let mut result = json!({
        "id": memory.id, "type": memory.memory_type.as_str(), "status": memory.status.as_str(),
        "content": memory.content, "score": item.score.total,
        "source": {
            "label": memory.source, "type": memory.provenance.source_type.as_str(),
            "uri": memory.provenance.source_uri, "hash": memory.provenance.source_hash,
            "span": memory.provenance.source_span, "created_by": memory.provenance.created_by,
        },
        "revision": {
            "historical": item.historical, "basis": basis, "number": number,
            "recorded_at": item.historical_recorded_at.unwrap_or(0),
            "as_of": item.historical_as_of.unwrap_or(0),
        },
        "token_cost": {
            "content_bytes": memory.content.len(),
            "estimated_input_tokens": memory.content.len().div_ceil(4),
            "exact": false, "method": "utf8_bytes_div_4", "scope": "memory_prose_only",
        },
        "score_breakdown": {
            "lexical": item.score.lexical, "hashed_similarity": item.score.hashed_similarity,
            "confidence": item.score.confidence, "quality": item.score.quality,
            "freshness": item.score.freshness, "scope": item.score.scope,
            "lexical_rank": item.score.lexical_rank, "graph_rank": item.score.graph_rank,
            "dense_rank": item.score.dense_rank,
            "dense_similarity": item.dense_match.as_ref().map(|hit| hit.similarity),
            "reciprocal_rank_fusion": item.score.reciprocal_rank_fusion,
        },
        "graph_path_ids": item.graph_path_ids,
        "graph_path_relations": item.graph_path_relations,
        "graph_path_reverse": item.graph_path_reverse,
        "graph_path": graph_path,
        "assertion_path": assertion_path,
        "connecting_memory": item.connecting_memory,
        "eligible_for_context": item.eligible_for_context,
        "historical": item.historical,
        "historical_revision": item.historical_revision,
        "warnings": item.warnings,
        "dense_match": null,
    });
    if let Some(hit) = &item.dense_match {
        result["dense_match"] = json!({
            "retriever": "dense", "rank": item.score.dense_rank, "similarity": hit.similarity,
            "index_generation": hit.generation, "chunk": chunk_json(&hit.chunk),
            "revision_is_current": current_revision.map(|revision| revision == hit.chunk.owner_revision),
        });
    }
    if let Some(pack) = preview {
        result["selection"] = selection_json(
            pack,
            &RetrievalReference::new(RetrievalOwnerKind::Memory, &memory.id),
            item.eligible_for_context,
        );
    }
    canonical(&result)
}

/// A typed memory/code/entity candidate with chunks, ranks, directed path,
/// assertion metadata and exclusion reasons.
pub fn retrieval_candidate_json(
    candidate: &RetrievalCandidate,
    preview: Option<&ContextPack>,
) -> Value {
    let chunks: Vec<Value> = candidate.chunks.iter().map(chunk_json).collect();
    let matched: Vec<Value> = candidate
        .matched_chunks
        .iter()
        .map(|hit| {
            json!({
                "retriever": "dense", "similarity": hit.similarity,
                "index_generation": hit.generation, "chunk": chunk_json(&hit.chunk),
            })
        })
        .collect();
    let ranks: Vec<Value> = candidate
        .ranks
        .iter()
        .map(|rank| json!({"retriever": rank.retriever, "rank": rank.rank, "score": rank.score}))
        .collect();
    let path: Vec<Value> = candidate
        .path
        .iter()
        .map(|edge| {
            let mut item = json!({
                "from": reference_json(&edge.from), "to": reference_json(&edge.to),
                "relation": edge.relation, "evidence_id": edge.evidence_id,
                "resolution": edge.resolution, "provenance": edge.provenance(),
                "heuristic": edge.heuristic,
            });
            if let Some(assertion) = &edge.assertion {
                item["assertion"] = assertion_json(assertion);
            }
            item
        })
        .collect();
    let mut result = json!({
        "owner": reference_json(&candidate.owner), "chunks": chunks, "matched_chunks": matched,
        "ranks": ranks, "path": path, "evidence_ids": candidate.evidence_ids,
        "eligible": candidate.eligible, "exclusion_reasons": candidate.exclusion_reasons,
        "fused_score": candidate.fused_score,
    });
    if let Some(pack) = preview {
        result["selection"] = selection_json(pack, &candidate.owner, candidate.eligible);
    }
    canonical(&result)
}

/// Describes a context pack without claiming it was injected or consumed.
/// Token and byte values are estimates of the complete rendered pack.
pub fn context_preview_json(
    pack: &ContextPack,
    token_budget: usize,
    bytes_per_token: usize,
) -> Value {
    let per_token = bytes_per_token.max(1);
    let bytes = pack.markdown.len();
    let mut selected_item_bytes = 0usize;
    let items: Vec<Value> = pack
        .item_explanations
        .iter()
        .map(|item| {
            if item.selected
                && let Some(rendered) = item.rendered_bytes
            {
                selected_item_bytes += rendered;
            }
            json!({
                "category": item.category, "memory_ids": item.memory_ids,
                "code_ids": item.code_ids, "block_ids": item.block_ids,
                "graph_bundle": item.graph_bundle, "selected": item.selected,
                "eligible": item.eligible, "reason": item.reason, "warnings": item.warnings,
                "rendered_bytes": item.rendered_bytes,
                "estimated_tokens": item.rendered_bytes.map(|rendered| rendered.div_ceil(per_token)),
                "cost_scope": "item_without_shared_envelope_or_heading",
                "exact_tokens": false,
            })
        })
        .collect();
    canonical(&json!({
        "items": items,
        "selected_item_bytes": selected_item_bytes,
        "shared_overhead_bytes": bytes.saturating_sub(selected_item_bytes),
        "selected": {
            "memory_ids": pack.memory_ids, "required_memory_ids": pack.required_memory_ids,
            "code_ids": pack.code_ids, "block_ids": pack.block_ids,
        },
        "injected": false,
        "used": null,
        "omitted": pack.omitted,
        "omission_reasons": pack.warnings,
        "token_budget": token_budget,
        "estimated_full_context": {
            "bytes": bytes, "tokens": bytes.div_ceil(per_token), "exact": false,
            "method": "rendered_utf8_bytes_div_bytes_per_token",
        },
    }))
}
