"""Exercise the production bridge with actual translated native page/dialog transitions.

No game entry point, launcher, device, platform window or gameplay is started.
XML tokenization, GSC factory cloning/lookup, string allocation, text animation,
event bus, UI list allocation, eligibility and host UI queue/backend are boundaries.
The menu loader's MainMenuPage-only branch, custom factory guard, input mask
translation, initial focus, linked navigation and selection execute translated code.
Plain text storage and translated text setters, native component construction/registration/name lookup, page selection and
event dispatch, GenericPage configure/reset/publish/dispatch, manager request,
frame/transition and root push/pop execute as translated TU23 code.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--production-only', action='store_true', help='Debug run only; do not produce a verification manifest')
    a = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    src, gen, sdk = root/'rexlego/src', root/'rexlego/generated/default', root.parent/'dev-assets/sdk-install'
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).with_name('native_settings_bridge_fixture.cpp')
    names = ['sub_82EFB478', 'sub_82EFB1E8', 'sub_82EF8D80', 'sub_82EF9730',
             'sub_82F0FF38', 'sub_82EF9560', 'sub_82EF92E8', 'sub_82EF51B0',
             'sub_82FB7428', 'sub_82FB74C8', 'sub_82FBDFF8', 'sub_82FBDF58',
             'sub_82FD0508', 'sub_82FD9270', 'sub_82EF61B0', 'sub_82EF61E0', 'sub_82EF42D8',
             'sub_82FD8C30', 'sub_82FEA350', 'sub_82FE3780', 'sub_82A7B0A8',
             'sub_82EFC738', 'sub_82F02940', 'sub_82F02A08']
    names += ['sub_82F0E590', 'sub_82EFD0E0', 'sub_82EFEEB8', 'sub_82EF9850',
              'sub_82EFBF70', 'sub_82EFD540', 'sub_82B3B3A8', 'sub_82F0E1B8',
              'sub_82F0DF30', '__savegprlr_17', '__restgprlr_17']
    names += ['sub_82A435C8','sub_82A30600','sub_82EFE8F0','sub_82784740','sub_82EFEFA0']
    names += ['sub_839C4D08','sub_82EF3D80','sub_82EDE1D0','sub_82F0E608','sub_82F0F958',
              '__savefpr_28','__restfpr_28']
    for number in range(22, 30):
        names += ['__savegprlr_'+str(number), '__restgprlr_'+str(number)]
    found = {}
    for path in gen.glob('legodimensions_recomp.*.cpp'):
        text = path.read_text()
        for name in names:
            start = text.find('DEFINE_REX_FUNC('+name+')')
            if start < 0:
                continue
            end = text.find('\nDEFINE_REX_FUNC(', start+1)
            found[name] = text[start:end if end >= 0 else len(text)]
    for name,alias in [('sub_82A435C8','NativeOriginalLocalize'),('sub_82EFE8F0','NativeTranslatedText'),('sub_82EFEFA0','NativePlainText')]:
        found[name] = found[name].replace('DEFINE_REX_FUNC('+name+')','DEFINE_REX_FUNC('+alias+')',1)
    assert set(found) == set(names)
    declarations = '\n'.join('void '+name+'(PPCContext&,uint8_t*);' for name in names)
    declarations += '\nvoid NativeOriginalLocalize(PPCContext&,uint8_t*);\nvoid NativeTranslatedText(PPCContext&,uint8_t*);\nvoid NativePlainText(PPCContext&,uint8_t*);'
    (out/'native-declarations.h').write_text(declarations)
    hooks = (src/'settings_native_hooks.cpp').read_text()
    start = hooks.index('void RefreshOptionsVisibility(')
    end = hooks.index('\nNativeSettingsBridge* GetBridge', start)
    (out/'native-adapters.inc').write_text(hooks[start:end])
    body = '\n'.join(found[n] for n in names)
    known = set(names) | set(re.findall(r'void (sub_\w+)\(PPCContext', fixture.read_text()))
    called = set(re.findall(r'\b(\w+)\(ctx, base\);', body))
    (out/'native-unexpected.inc').write_text('\n'.join('void '+n+'(PPCContext&,uint8_t*){CHECK(false);}' for n in sorted(called-known)))
    (out/'native-translated.inc').write_text(body)
    for name in ['rexruntime.dll', 'amd_fidelityfx_dx12.dll']:
        shutil.copy2(root.parent/'build/worlds-candidate-v18'/name, out/name)
    flags = ['-std=c++23', '-O2', '-mavx2', '-DSPDLOG_FMT_EXTERNAL', '-Wno-unused-value',
             '-Wno-deprecated-literal-operator', '-D_CRT_SECURE_NO_WARNINGS', '-DWIN32_LEAN_AND_MEAN',
             '-DNOMINMAX', '-I'+str(out), '-I'+str(gen), '-I'+str(src), '-I'+str(sdk/'include')]
    objects = []
    for source in [fixture, *[src/n for n in ['settings_frontend.cpp', 'settings_registry.cpp',
                    'settings_service.cpp', 'settings_transaction.cpp', 'settings_config.cpp']]]:
        obj = out/(source.stem+'.obj')
        result = subprocess.run([a.compiler, *flags, '-c', str(source), '-o', str(obj)], capture_output=True, text=True, timeout=90)
        (out/(source.stem+'-compile.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        objects.append(obj)
    production = src/'settings_native_bridge.cpp'

    def run(label, source=production, frontend=None, adapter=None):
        obj, exe = out/(label+'.obj'), out/(label+'.exe')
        result = subprocess.run([a.compiler, *flags, '-c', str(source), '-o', str(obj)], capture_output=True, text=True, timeout=90)
        (out/(label+'-compile.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        linked = objects.copy()
        if adapter:
            include = out/'native-adapters.inc';saved = include.read_text()
            try:
                include.write_text(adapter)
                fixture_obj = out/(label+'-fixture.obj')
                result = subprocess.run([a.compiler,*flags,'-c',str(fixture),'-o',str(fixture_obj)],capture_output=True,text=True,timeout=90)
                (out/(label+'-fixture-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
                linked[0] = fixture_obj
            finally:
                include.write_text(saved)
        if frontend:
            front_obj = out/(label+'-frontend.obj')
            result = subprocess.run([a.compiler, *flags, '-c', str(frontend), '-o', str(front_obj)], capture_output=True, text=True, timeout=90)
            (out/(label+'-frontend-compile.log')).write_text(result.stdout+result.stderr)
            result.check_returncode()
            linked[1] = front_obj
        result = subprocess.run([a.compiler, *map(str, linked), str(obj), str(sdk/'lib/rexruntime.lib'), '-o', str(exe)], capture_output=True, text=True, timeout=75)
        (out/(label+'-link.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe), str(a.image.resolve())], cwd=out, capture_output=True, text=True, timeout=25, creationflags=subprocess.CREATE_NO_WINDOW)
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        return result

    positive = run('production')
    positive.check_returncode()
    print(positive.stdout, end='', flush=True)
    if a.production_only:
        return
    text = production.read_text()
    negative = []
    for label, old, new, expected_count in [
        ('quit-hidden-cancel-default', 'if (configured) FocusPopup(ctx, base, 0);', 'if (configured) FocusPopup(ctx, base, quit ? 1 : 0);', 1),
        ('missing-value-alias', '  literal_value_aliases_.try_emplace(value_address, entry);', '  (void)value_address;', 1),
        ('foreign-English-matching', '  constexpr std::string_view prefix = "NATIVE_FRONTEND_";',
         '  {std::lock_guard lock(literal_mutex_);if(auto it=literal_values_.find(reinterpret_cast<const char*>(base+key));it!=literal_values_.end())return it->second;}\n  constexpr std::string_view prefix = "NATIVE_FRONTEND_";', 1),
        ('swapped-page-components', '{pages_[0].component, pages_[1].component, generic_}', '{pages_[1].component, pages_[0].component, generic_}', 1),
        ('no-model-poll', '  if (controller_) controller_->Poll();\n  Sync(ctx, base);', '  Sync(ctx, base);', 1),
        ('same-popup-labels', 'Store(base, labels_ + 4, b);', 'Store(base, labels_ + 4, a);', 2),
        ('no-exit-observation', 'ObserveExit(base);', '(void)base;', 4),
        ('rebuild-without-cancel', 'if (controller_ && !controller_->exit_requested()) controller_->Leave();', '(void)manager;', 1),
        ('reuse-retired-component', 'page.retired || ', '', 2),
        ('reuse-old-dialog-identity', 'interface_ = Allocate(base, 8); labels_ = Allocate(base, 8);', 'if (!interface_) interface_ = Allocate(base, 8); labels_ = Allocate(base, 8);', 1),
        ('no-recovery-page-remap', 'controller_->RebindPage(page->id);', '(void)page;', 2),
        ('restoration-still-actionable', 'view.buttons.empty() ? native_frontend::PopupButtons::kNone :',
         'false ? native_frontend::PopupButtons::kNone :', 1),
        ('skip-live-dialog-reset', 'CallGuest(api_.reset_popup, ctx, base, generic_);', '(void)generic_;', 2),
        ('forget-visibility-mask', 'Store(base, list + 16, visible);', 'Store(base, list + 16, 0);', 1),
        ('quit-on-cancel', 'extra_ == Extra::kQuit && choice == 0', 'extra_ == Extra::kQuit && choice == 1', 1),
        ('skip-custom-menu-factory', 'return CallGuest(api_.construct_menu, ctx, base, Load(base, factory + 4), xml_node, parent);', 'return 0;', 1),
        ('unscoped-menu-factory', '!loading_layout_ || !api_.construct_menu', '!api_.construct_menu', 1),
        ('skip-owned-input', 'if (!page) return false; // GenericPage', 'if (page) return false; // GenericPage', 1),
        ('wrong-input-direction', 'directions & 1 ? 9u : directions & 4 ? 10u', 'directions & 1 ? 10u : directions & 4 ? 9u', 1),
        ('missing-owned-localization', 'LiteralEntry{key_address, value_address}', 'LiteralEntry{key_address, 0}', 1),
        ('wrong-input-layer', 'layer != 2 || !packet', 'layer != 0 || !packet', 1),
        ('no-live-page-validation', 'LivePage(base, pages_[0]) && LivePage(base, pages_[1]) && RegisteredPages()', 'true', 1),
        ('no-suspended-request', 'SuspendRequest(base, manager);', '(void)manager;', 2),
        ('wrong-menu-sound', 'CallGuest(api_.play_sound, ctx, base, ui_,', 'CallGuest(api_.play_sound, ctx, base, ui_, 3 +', 1),
        ('missing-navigation-cue', 'Focus(ctx, base, *page, (row + page->rows.size() + (event == 9 ? -1 : 1)) % page->rows.size());', 'Focus(ctx, base, *page, (row + page->rows.size() + (event == 9 ? -1 : 1)) % page->rows.size()); return true;', 1),
        ('duplicate-navigation-cue', '// that it does not request a move sound for these owned pages.\n    PlaySound(ctx, base, 0);', '// that it does not request a move sound for these owned pages.\n    PlaySound(ctx, base, 0); PlaySound(ctx, base, 0);', 1),
        ('eager-loading-rebuild', 'pending_manager_ && options_live && (setup_requested_ || resume_page_ != UINT32_MAX)', 'pending_manager_', 1),
        ('missing-generic-accept', 'if (current == 23 && Load(base, generic_ + 176)', 'if (false && current == 23 && Load(base, generic_ + 176)', 1),
        ('v25-popup-child-focus', 'const auto root = Load(base, generic_ + 28), widget =', 'const auto root = Load(base, generic_ + 36), widget =', 1),
        ('forget-extra-return-focus', 'RestoreExtraFocus(ctx, base);', '(void)base;', 1),
        ('consume-focus-before-root-ready', '  const auto root = LiveMainMenuRoot(ctx, base, manager_, extra_return_);', '  extra_focus_pending_ = false;\n  const auto root = LiveMainMenuRoot(ctx, base, manager_, extra_return_);', 1),
        ('wrong-pause-return', 'pause_return_ = 0;', 'pause_return_ = 19;', 1),
        ('legacy-input-leak', 'Store(base, menu + offset, 0);', 'Store(base, menu + offset, 1);', 1),
        ('wrong-pause-back-index', 'Store(base, menu + 64, pause_video_row_);', 'Store(base, menu + 64, row);', 2),
        ('missing-pause-entries', 'if (!draw_row || !LivePauseOptions(base, menu))', 'if (true || !draw_row || !LivePauseOptions(base, menu))', 1),
        ('lose-deferred-main-click', '    if (LiveExtraSource(ctx, base, manager)) TryShowMenuExtra', '    if (false && LiveExtraSource(ctx, base, manager)) TryShowMenuExtra', 1),
        ('v33-child-as-page-root', 'const auto root = Load(controller.data(), 40), menu = Load(base, 0x849F5F40);', 'const auto root = Load(base, 0x849F5F40), menu = Load(base, 0x849F5F40);', 1),
        ('unrelated-main-menu-child', '== menu ? root : 0;', '!= 0 ? root : 0;', 1),
        ('bootstrap-during-main-transition', '  if (Load(base, manager + 168) != UINT32_MAX || Load(base, manager + 172) != UINT32_MAX) return 0;', '', 1),
        ('oversize-gameplay-options', ') * 0.70f)', ') * 1.0f)', 1),
    ]:
        assert text.count(old) == expected_count, (label, text.count(old))
        mutant = out/(label+'.cpp')
        mutant_text = text.replace(old, new)
        if label == 'reuse-retired-component':
            # Retirement now has a second independent barrier: remove the
            # owned dead node from the live registry before any name lookup.
            # Disable both guards to model the former stale-component reuse.
            assert mutant_text.count('page.retired && node == page.root') == 1
            mutant_text = mutant_text.replace('page.retired && node == page.root', 'false && node == page.root')
        mutant.write_text(mutant_text)
        result = run(label, mutant)
        # Removing request suspension can reach translated Show with the
        # observed zero-vtable root before the fixture's own assertions.
        expected_exits = (23, 0xC0000005) if label in ('no-suspended-request', 'v25-popup-child-focus') else (23,)
        assert result.returncode in expected_exits, (label, result.returncode, result.stdout, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    frontend_text = (src/'settings_frontend.cpp').read_text()
    adapter_text = (out/'native-adapters.inc').read_text()
    for label,old,new in [
        ('v30-raw-keys-in-visible-text', 'ctx, base, widget, text);', 'ctx, base, widget, key);'),
        ('duplicate-child-setter', '  native_frontend::CallGuest(__imp__sub_82EFEFA0, ctx, base, widget, text);',
         '  native_frontend::CallGuest(__imp__sub_82EFEFA0, ctx, base, widget, text);\n  const auto child = native_frontend::component_detail::Load(base, widget + 536);\n  if (child) native_frontend::CallGuest(sub_82EFE8F0, ctx, base, child, key);'),
    ]:
        assert adapter_text.count(old)==1,(label,adapter_text.count(old))
        result=run(label,adapter=adapter_text.replace(old,new))
        assert result.returncode==23,(label,result.returncode,result.stdout,result.stderr)
        negative.append(dict(name=label,exit=result.returncode,error=result.stderr))
    old = 'native_frontend::CallGuest(sub_82EFD540, ctx, base, list);'
    assert adapter_text.count(old)==1
    broken_adapter = adapter_text.replace(old,'native_frontend::CallGuest(sub_82F0F958, ctx, base, list);')
    result = run('reclone-options-on-entry',adapter=broken_adapter)
    assert result.returncode==23, (result.returncode,result.stdout,result.stderr)
    negative.append(dict(name='reclone-options-on-entry',exit=result.returncode,error=result.stderr))
    for label, old, new in [
        ('missing-native-menu', '<menu name=', '<disabledmenu name='),
    ]:
        assert old in frontend_text
        mutant = out/(label+'-frontend.cpp')
        mutant.write_text(frontend_text.replace(old,new))
        result = run(label, frontend=mutant)
        assert result.returncode == 23, (label,result.returncode,result.stdout,result.stderr)
        negative.append(dict(name=label,exit=result.returncode,error=result.stderr))
    sources = [src/n for n in ['settings_native_bridge.cpp', 'settings_native_bridge.h', 'settings_native_hooks.cpp',
        'settings_frontend.cpp', 'settings_frontend.h', 'native_frontend_binding.h', 'native_frontend_component.h',
        'native_frontend_guest.h', 'native_frontend_layout.h', 'settings_registry.cpp', 'settings_registry.h',
        'settings_service.cpp', 'settings_service.h', 'settings_transaction.cpp', 'settings_transaction.h']]
    sources += [fixture, Path(__file__).resolve()]
    (out/'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False,
        output=positive.stdout, sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        translated_sha256={n:hashlib.sha256(found[n].encode()).hexdigest() for n in names}, negative=negative,
        scope=__doc__, limits=['Native hooks adapter compiled separately; actual SDK heap/thunk allocator and UI context are injected boundaries here.',
        'Rendered native pages, real controller/keyboard/mouse navigation, real GSC/registry teardown and renderer/window acknowledgement/owner gameplay remain unverified. CPU recreation cases model the title retaining or clearing its registries.']), indent=2))


if __name__ == '__main__':
    main()
