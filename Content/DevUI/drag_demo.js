// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.
//
// VaCuus drag'n'drop demo -- the JS half of DevUI/drag_demo.rml. RmlUi owns the
// drag MECHANICS (source selection, the ghost clone, the event stream); this
// file owns the POLICY: which drops are legal, what moves where, and every
// piece of visual feedback, because RmlUi provides none for targets.
//
// THE EVENT CONTRACT, verified against the vendored tree:
//
//  - dragstart/drag/dragend fire on the SOURCE item; dragover/dragout/dragdrop
//    fire on what the cursor is over, and ONLY because .item is `drag: clone`
//    -- of the five `drag` values just drag-drop and clone enable the verbose
//    set (Context.cpp:692).
//  - dragdrop fires on the drop TARGET and bubbles; the source element is NOT
//    in the event -- RmlUi passes it as the `drag_element` void*, which the JS
//    bridge deliberately drops (VaCuusJsEvents.cpp:44-49) -- so `dragged`
//    below, captured at dragstart, is the one reliable handle to it.
//  - the release sequence over a target is dragdrop -> dragout -> dragend
//    (Context.cpp:760-775): the target is told twice, the source once, and
//    dragend fires EVEN when the drop landed nowhere. That is why dragend is
//    the single place this file mutates the tree and restores invariants.
//  - there is NO movement threshold: dragstart fires on the first >= 1px move
//    with the button held (Context.cpp:1290), so a sloppy click can start a
//    drag. Fine for a demo; a shipping UI that cares should debounce in its
//    dragstart handler.
//
// WHY THE MOVE HAPPENS IN dragend AND MUST NOT HAPPEN IN dragdrop: the facade's
// appendChild moves an attached node by RemoveChild from its old parent
// (VaCuusJsDom.cpp InsertThunk), and detaching the element RmlUi currently
// considers dragged runs Element::SetOwnerDocument's OnElementDetach
// (Element.cpp:2129-2133) -- whose drag branch SILENTLY CANCELS the drag and
// suppresses dragend (Context.cpp:1150-1159). Reparent inside dragdrop and the
// 'dragging' class sticks forever because the cleanup event never comes. In
// dragend the same cancellation runs but everything it kills has already
// happened, so the move is safe there -- dragdrop VALIDATES, dragend APPLIES.
//
// TARGET-SIDE FILTERING, the subtle half: dragover/dragout are dispatched to
// every element ENTERING/LEAVING the drag-hover chain and they bubble, so a
// slot's handler also hears its occupying item's transitions. The highlight
// handlers accept only ev.target === ev.currentTarget (the slot's own
// membership change); the dragdrop handler deliberately does NOT filter,
// because a drop over an occupied slot lands on the ITEM (the deepest element
// under the cursor, Context.cpp:1347's GetElementAtPoint excludes only the
// dragged element itself) and reaches the slot by bubbling -- filtering there
// would make occupied slots drop-dead.
'use strict';

var dragged = null;		// the .item being dragged, captured at dragstart
var sourceSlot = null;	// its slot of origin
var verdict = null;		// {kind: 'moved'|'kept'|'refused', slot} -- written by dragdrop, applied by dragend

// Test observable (VaCuus.Js.DragDrop): the slot-level event order, 'type:id'.
globalThis.dragLog = [];

function itemIn(slot)
{
	return slot.querySelector('.item');
}

/** A drop is legal into an empty matching slot; "back where it came from" is
 *  also legal so a changed mind is a no-op, not a refusal. */
function canDrop(slot, item)
{
	var occupant = itemIn(slot);
	if (occupant !== null)
	{
		return occupant === item;
	}
	var accepts = slot.getAttribute('accepts');
	return accepts === 'any' || accepts === item.getAttribute('itemtype');
}

function setStatus(text)
{
	// The mirror global is the automation test's readback: what innerRML gives
	// BACK is serialized markup, so a status containing '->' would round-trip
	// escaped and the assertion would be about XML escaping, not the demo.
	globalThis.lastStatus = text;
	document.getElementById('status').innerRML = text;
}

function clearHighlights()
{
	var slots = document.body.querySelectorAll('.slot');
	for (var i = 0; i < slots.length; i++)
	{
		slots[i].classList.remove('drop-ok');
		slots[i].classList.remove('drop-bad');
	}
}

function onDragStart(ev)
{
	dragged = ev.currentTarget;
	sourceSlot = dragged.parentNode;
	verdict = null;
	dragged.classList.add('dragging');
	setStatus('DRAGGING ' + dragged.id.toUpperCase());
	dragLog.push('dragstart:' + dragged.id);
}

function onDragOver(ev)
{
	if (ev.target !== ev.currentTarget || dragged === null)
	{
		return;
	}
	var slot = ev.currentTarget;
	slot.classList.add(canDrop(slot, dragged) ? 'drop-ok' : 'drop-bad');
	dragLog.push('dragover:' + slot.id);
}

function onDragOut(ev)
{
	if (ev.target !== ev.currentTarget)
	{
		return;
	}
	ev.currentTarget.classList.remove('drop-ok');
	ev.currentTarget.classList.remove('drop-bad');
	dragLog.push('dragout:' + ev.currentTarget.id);
}

function onDragDrop(ev)
{
	// Validate only -- NO tree mutation here, see the header. currentTarget is
	// the slot whether the drop landed on it directly or on its occupant.
	var slot = ev.currentTarget;
	if (dragged === null)
	{
		return;
	}
	dragLog.push('dragdrop:' + slot.id);
	if (!canDrop(slot, dragged))
	{
		verdict = {kind: 'refused', slot: slot};
	}
	else if (slot !== sourceSlot)
	{
		verdict = {kind: 'moved', slot: slot};
	}
	else
	{
		verdict = {kind: 'kept', slot: slot};
	}
}

function onDragEnd(ev)
{
	// Always the last drag event (Context.cpp:772), whatever the drop did or
	// whether there was one -- the one safe place to mutate and clean up.
	var item = ev.currentTarget;
	clearHighlights();
	item.classList.remove('dragging');

	if (verdict !== null && verdict.kind === 'moved')
	{
		verdict.slot.appendChild(item);
		setStatus('MOVED ' + item.id.toUpperCase() + ' -> ' + verdict.slot.id.toUpperCase());
	}
	else if (verdict !== null && verdict.kind === 'kept')
	{
		setStatus('KEPT ' + item.id.toUpperCase());
	}
	else if (verdict !== null)
	{
		setStatus('REFUSED ' + item.id.toUpperCase() + ' x ' + verdict.slot.id.toUpperCase());
	}
	else
	{
		setStatus('RETURNED ' + item.id.toUpperCase());
	}

	dragLog.push('dragend:' + item.id);
	dragged = null;
	sourceSlot = null;
	verdict = null;
}

function boot()
{
	var items = document.body.querySelectorAll('.item');
	for (var i = 0; i < items.length; i++)
	{
		// Listeners ride the NODE, so they survive appendChild reparenting and
		// an equipped item can be dragged back without rewiring.
		items[i].addEventListener('dragstart', onDragStart);
		items[i].addEventListener('dragend', onDragEnd);
	}

	var slots = document.body.querySelectorAll('.slot');
	for (var j = 0; j < slots.length; j++)
	{
		slots[j].addEventListener('dragover', onDragOver);
		slots[j].addEventListener('dragout', onDragOut);
		slots[j].addEventListener('dragdrop', onDragDrop);
	}
}

boot();
