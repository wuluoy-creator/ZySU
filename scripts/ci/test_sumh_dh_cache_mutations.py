"""Narrow faults in generated actual bodies; compilation failure never passes."""
from test_sumh_dh_ownership_source import function, replace_once


def scoped(source, marker, old, new):
    body = function(source, marker)
    return replace_once(source, body, replace_once(body, old, new))


def mutations(make_source):
    text = make_source(scenario="first_publication")
    text = scoped(text, "static int sumh_dh_prepare_observed(", "inode_lock(inode);", "/* injected missing barrier */")
    text = scoped(text, "static int sumh_dh_prepare_observed(", "inode_unlock(inode);", "/* injected missing barrier */")
    yield "missing-parent-barrier", text, "inode_lock_calls==1&&inode_unlock_calls==1&&first_native_complete&&first_invalidation>first_native_complete"

    text = make_source(scenario="transparent_prepared")
    text = scoped(text, "static void sumh_dh_native_complete(", "sumh_dh_find_slot(", "sumh_dh_find_child(")
    yield "attached-only-native-completion", text, "governed(actual)&&validate(actual)==1&&actual->hashed"

    text = make_source(scenario="mapped_atomic_open")
    text = scoped(text, "static int SUMH_NOCFI sumh_dh_atomic_open_inner(",
                  "if (sumh_dh_capture_projection(dn, dir, name, len, &projection))", "if (false && sumh_dh_capture_projection(dn, dir, name, len, &projection))")
    yield "native-mapped-atomic-open", text, "result==0&&native_opens==0&&native_create_count==0&&finish_calls==1"

    text = make_source(scenario="mapped_atomic_open")
    text = scoped(text, "static int SUMH_NOCFI sumh_dh_atomic_open_inner(", "d_drop(dentry);", "/* injected hashed input */")
    yield "hashed-projection-input", text, "!rehash_errors&&!(f.f_mode&(FMODE_OPENED|FMODE_CREATED))"

    text = make_source(scenario="mapped_atomic_open")
    text = scoped(text, "static int SUMH_NOCFI sumh_dh_atomic_open_inner(",
                  "return finish_no_open(file, result);", "if (result && !IS_ERR(result)) dput(result);\n\t\treturn finish_no_open(file, result);")
    yield "double-consumed-alias", text, "!alias||actual->refs==3"

    text = make_source(scenario="stale_completion")
    text = scoped(text, "static bool\nsumh_dh_projection_matches(",
                  "view.payload != projection->payload ||", "/* injected missing captured identity */")
    yield "uncaptured-source-identity", text, "IS_ERR(res)&&native_lookups==0&&!d->hashed"

    text = make_source(scenario="retire_before_publication")
    text = scoped(text, "static struct dentry *\nsumh_dh_materialize(",
                  "if (!sumh_dh_validate_projection(dn, res ?: dentry, projection))", "if (false && !sumh_dh_validate_projection(dn, res ?: dentry, projection))")
    yield "missing-final-publication-validation", text, "result<0&&!actual->hashed&&native_lookups==0&&native_opens==0"

    text = make_source(scenario="capture_pin_survives_detach")
    text = scoped(text, "static bool sumh_dh_capture_projection(",
                  "refcount_inc(&view.payload->refs);", "(void)view.payload;")
    yield "missing-captured-payload-pin", text, "captured.payload==old&&refcount_read(&old->refs)==2"

    text = make_source(scenario="payload_pin_prevents_reuse")
    text = scoped(text, "static int sumh_dh_set_dentry_ops(", "refcount_inc(&m->payload->refs);", "(void)m->payload;")
    yield "missing-metadata-payload-pin", text, "refcount_read(&old->refs)==2"

    text = make_source(scenario="immutable_meta_reuse")
    text = scoped(text, "static int sumh_dh_set_dentry_ops(", "existing->projected != m->projected ||", "")
    text = scoped(text, "static int sumh_dh_set_dentry_ops(", "existing->synthetic_negative !=\n\t\t\t     m->synthetic_negative ||", "")
    text = scoped(text, "static int sumh_dh_set_dentry_ops(", "existing->payload != m->payload", "false")
    yield "mutable-projection-classification", text, "sumh_dh_set_dentry_ops(a->slot->dir,d,NULL)==-EBUSY"

    text = make_source(scenario="native_metadata_failure")
    text = scoped(text, "static void sumh_dh_native_complete(",
                  "if (!same_name || sumh_dh_set_dentry_ops(dn, result, NULL))\n\t\tsumh_dh_drop_projection(result);",
                  "if (!same_name || sumh_dh_set_dentry_ops(dn, result, NULL))\n\t\t(void)result;")
    yield "raw-native-result-after-install-failure", text, "!IS_ERR(res)&&!d->hashed&&!governed(d)&&d_inode(d)==&fixture_inodes[18]"

    text = make_source(scenario="observer_transition_failed_materialize")
    body = function(text, "static struct dentry *\nsumh_dh_materialize(")
    before, label, after = body.partition("failed_input:")
    if not label:
        raise AssertionError("missing failed_input cleanup label")
    changed = before + label + replace_once(after, "sumh_dh_drop_projection(dentry);", "/* injected missing input retirement */")
    text = replace_once(text, body, changed)
    yield "leaked-native-meta-after-observer-transition", text, "!governed(d)&&d->refs==1&&a->state==DH_ATTACHED&&winner(1,\"file\")==501"
