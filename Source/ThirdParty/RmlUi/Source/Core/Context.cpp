#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/ContextInstancer.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/DataModelHandle.h"
#include "../../Include/RmlUi/Core/Debug.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/StreamMemory.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "DataModel.h"
#include "EventDispatcher.h"
#include "PluginRegistry.h"
#include "ScrollController.h"
#include "StreamFile.h"
#include <algorithm>
#include <clocale>
#include <iterator>
#include <limits>

namespace Rml {

static constexpr float DOUBLE_CLICK_TIME = 0.5f;    // [s]
static constexpr float DOUBLE_CLICK_MAX_DIST = 3.f; // [dp]
static constexpr float UNIT_SCROLL_LENGTH = 80.f;   // [dp]

// If the user stops scrolling for this amount of time in seconds before touch/click release, don't apply inertia.
static constexpr float SCROLL_INERTIA_DELAY = 0.1f;
static constexpr float TOUCH_MOVEMENT_DECAY_RATE = 5.0f;
static constexpr float TOUCH_CLICK_MAX_DISTANCE = DOUBLE_CLICK_MAX_DIST; // [dp]

static void DebugVerifyLocaleSetting()
{
#ifdef RMLUI_DEBUG
	constexpr float expected_value = 1000.5f;
	const Rml::String expected_string = "1000.5";
	const Rml::String formatted_string = Rml::ToString(expected_value);
	const float parsed_value = Rml::FromString<float>(expected_string);

	const char* description = "RmlUi expects the global locale to be set to the default minimal \"C\" locale, please see `std::setlocale`.";
	if (formatted_string != expected_string)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR,
			"Incompatible locale setting detected while formatting %f. Formatted: \"%s\". Expected: \"%s\". Current locale: %s. %s", expected_value,
			formatted_string.c_str(), expected_string.c_str(), std::setlocale(LC_ALL, nullptr), description);
	}
	if (parsed_value != expected_value)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR,
			"Incompatible locale setting detected while parsing \"%s\". Parsed: %f. Expected: %f. Current locale: %s. %s", expected_string.c_str(),
			parsed_value, expected_value, std::setlocale(LC_ALL, nullptr), description);
	}
#endif
}

Context::Context(const String& name, RenderManager* render_manager, TextInputHandler* text_input_handler) :
	name(name), render_manager(render_manager), text_input_handler(text_input_handler)
{
	instancer = nullptr;

	root = Factory::InstanceElement(nullptr, "*", "#root", XMLAttributes());
	root->SetId(name);
	root->SetOffset(Vector2f(0, 0), nullptr);
	root->SetProperty(PropertyId::ZIndex, Property(0, Unit::NUMBER));

	document_focus_history.push_back(root.get());
	focus = root.get();
	enable_cursor = true;

	scroll_controller = MakeUnique<ScrollController>();
}

ElementPtr Context::CreateCursorProxy()
{
	// Keep a document instancer even when the application changes the document's base tag.
	ElementPtr proxy = Factory::InstanceElement(nullptr, "body", documents_base_tag, XMLAttributes());
	ElementDocument* cursor_proxy_document = rmlui_dynamic_cast<ElementDocument*>(proxy.get());
	RMLUI_ASSERT(cursor_proxy_document);
	cursor_proxy_document->context = this;

	// The cursor proxy takes the style from its cloned element's document. The latter may define style rules for `<body>` which we don't want on the
	// proxy. Thus, we override some properties here that we in particular don't want to inherit from the client document, especially those that
	// result in decoration of the body element.
	cursor_proxy_document->SetProperty(PropertyId::BackgroundColor, Property(Colourb(255, 255, 255, 0), Unit::COLOUR));
	cursor_proxy_document->SetProperty(PropertyId::BorderTopWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderRightWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderBottomWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderLeftWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::Decorator, Property());
	cursor_proxy_document->SetProperty(PropertyId::OverflowX, Property(Style::Overflow::Visible));
	cursor_proxy_document->SetProperty(PropertyId::OverflowY, Property(Style::Overflow::Visible));
	return proxy;
}

Context::~Context()
{
	PluginRegistry::NotifyContextDestroy(this);

	UnloadAllDocuments();

	ReleaseUnloadedDocuments();

	for (const auto& pair : data_models)
		PluginRegistry::NotifyDataModelDestroy(this, pair.first);
	data_models.clear();

	root.reset();

	for (PointerState* state : {&mouse_pointer, &touch_pointer})
	{
		PointerScope scope(*this, *state);
		pointer->cursor_proxy.reset();
	}

	instancer = nullptr;
}

const String& Context::GetName() const
{
	return name;
}

void Context::SetDimensions(const Vector2i _dimensions)
{
	if (dimensions != _dimensions)
	{
		dimensions = _dimensions;
		render_manager->SetViewport(dimensions);
		root->SetBox(Box(Vector2f(dimensions)));
		root->DirtyLayout();

		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document != nullptr)
			{
				document->DirtyMediaQueries();
				document->DirtyVwAndVhProperties();
				document->DirtyLayout();
				document->DirtyPosition();
				document->DispatchEvent(EventId::Resize, Dictionary());
			}
		}
	}
}

Vector2i Context::GetDimensions() const
{
	return dimensions;
}

void Context::SetDensityIndependentPixelRatio(float dp_ratio)
{
	if (density_independent_pixel_ratio != dp_ratio)
	{
		density_independent_pixel_ratio = dp_ratio;

		for (int i = 0; i < root->GetNumChildren(true); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document)
			{
				document->DirtyMediaQueries();
				document->OnDpRatioChangeRecursive();
			}
		}
	}
}

float Context::GetDensityIndependentPixelRatio() const
{
	return density_independent_pixel_ratio;
}

bool Context::Update()
{
	RMLUI_ZoneScoped;
	DebugVerifyLocaleSetting();

	next_update_timeout = std::numeric_limits<double>::infinity();

	if (scroll_controller->Update(pointer->mouse_position, density_independent_pixel_ratio))
		RequestNextUpdate(0);

	// Layout changes can affect either pointer while the other is idle.
	for (PointerState* state : {&mouse_pointer, &touch_pointer})
	{
		PointerScope scope(*this, *state);
		if (pointer->mouse_active)
			UpdateHoverChain(pointer->mouse_position);
	}

	// Update all the data models before updating properties and layout.
	for (auto& data_model : data_models)
		data_model.second->Update(true);

	// The style definition of each document should be independent of each other. By manually resetting these flags we avoid unnecessary definition
	// lookups in unrelated documents, such as when adding a new document. Adding an element dirties the parent definition, which in this case is the
	// root. By extension the definition of all the other documents are also dirtied, unnecessarily.
	root->dirty_definition = false;
	root->dirty_child_definitions = false;

	root->Update(density_independent_pixel_ratio, Vector2f(dimensions));

	for (int i = 0; i < root->GetNumChildren(); ++i)
	{
		if (auto doc = root->GetChild(i)->GetOwnerDocument())
		{
			doc->UpdateLayout();
			doc->UpdatePosition();
		}
	}

	// Release any documents that were unloaded during the update.
	ReleaseUnloadedDocuments();

	return true;
}

bool Context::Render()
{
	RMLUI_ZoneScoped;

	render_manager->PrepareRender(dimensions);

	root->Render();

	for (PointerState* state : {&mouse_pointer, &touch_pointer})
		RenderDragCloneProxy(state->cursor_proxy.get(), Vector2f(state->mouse_position));
	for (auto& entry : secondary_touch_states)
		RenderDragCloneProxy(entry.second.drag_proxy.get(), entry.second.last_position);

	render_manager->ResetState();

	return true;
}

ElementDocument* Context::CreateDocument(const String& instancer_name)
{
	ElementPtr element = Factory::InstanceElement(nullptr, instancer_name, documents_base_tag, XMLAttributes());
	if (!element)
	{
		Log::Message(Log::LT_ERROR, "Failed to instance document on instancer_name '%s', instancer returned nullptr.", instancer_name.c_str());
		return nullptr;
	}

	ElementDocument* document = rmlui_dynamic_cast<ElementDocument*>(element.get());
	if (!document)
	{
		Log::Message(Log::LT_ERROR,
			"Failed to instance document on instancer_name '%s', Found type '%s', was expecting derivative of ElementDocument.",
			instancer_name.c_str(), rmlui_type_name(*element));
		return nullptr;
	}

	document->context = this;
	root->AppendChild(std::move(element));

	PluginRegistry::NotifyDocumentLoad(document);

	return document;
}

ElementDocument* Context::LoadDocument(const String& document_path)
{
	auto stream = MakeUnique<StreamFile>();

	if (!stream->Open(document_path))
		return nullptr;

	ElementDocument* document = LoadDocument(stream.get());

	return document;
}

ElementDocument* Context::LoadDocument(Stream* stream)
{
	DebugVerifyLocaleSetting();
	PluginRegistry::NotifyDocumentOpen(this, stream->GetSourceURL().GetURL());

	ElementPtr element = Factory::InstanceDocumentStream(this, stream, GetDocumentsBaseTag());
	if (!element)
		return nullptr;

	ElementDocument* document = rmlui_static_cast<ElementDocument*>(element.get());

	root->AppendChild(std::move(element));

	// The 'load' event is fired before updating the document, because the user might
	// need to initalize things before running an update. The drawback is that computed
	// values and layouting are not performed yet, resulting in default values when
	// querying such information in the event handler.
	PluginRegistry::NotifyDocumentLoad(document);
	document->DispatchEvent(EventId::Load, Dictionary());

	// Data models are updated after the 'load' event so that the user has a chance to change
	// any data variables first. We do not clear dirty variables here, since users may need to
	// retrieve whether or not eg. a data variable has changed in a controller.
	for (auto& data_model : data_models)
		data_model.second->Update(false);

	document->UpdateDocument();

	return document;
}

ElementDocument* Context::LoadDocumentFromMemory(const String& string, const String& source_url)
{
	// Open the stream based on the string contents.
	auto stream = MakeUnique<StreamMemory>(reinterpret_cast<const byte*>(string.c_str()), string.size());

	stream->SetSourceURL(source_url);

	// Load the document from the stream.
	ElementDocument* document = LoadDocument(stream.get());

	return document;
}

void Context::UnloadDocument(ElementDocument* _document)
{
	// Has this document already been unloaded?
	for (size_t i = 0; i < unloaded_documents.size(); ++i)
	{
		if (unloaded_documents[i].get() == _document)
			return;
	}

	ElementDocument* document = _document;

	if (document->GetParentNode() == root.get())
	{
		// Dispatch the unload notifications.
		document->DispatchEvent(EventId::Unload, Dictionary());
		PluginRegistry::NotifyDocumentUnload(document);

		// Move document to a temporary location to be released later.
		unloaded_documents.push_back(root->RemoveChild(document));
	}

	// Remove the item from the focus history.
	ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), document);
	if (itr != document_focus_history.end())
		document_focus_history.erase(itr);

	// Focus to the previous document if the old document is the current focus.
	if (focus && focus->GetOwnerDocument() == document)
	{
		focus = nullptr;
		document_focus_history.back()->GetFocusLeafNode()->Focus();
	}

	for (PointerState* state : {&mouse_pointer, &touch_pointer})
	{
		PointerScope scope(*this, *state);
		// Clear the active element if the old document is the active element.
		if (pointer->active && pointer->active->GetOwnerDocument() == document)
		{
			pointer->active = nullptr;
		}

		// Clear other pointers to elements whose owner was deleted
		if (pointer->drag && pointer->drag->GetOwnerDocument() == document)
		{
			pointer->drag = nullptr;
			ReleaseDragClone();
		}

		if (pointer->drag_hover && pointer->drag_hover->GetOwnerDocument() == document)
		{
			pointer->drag_hover = nullptr;
		}

	}

	// VaCuus patch #18: a secondary finger keeps its pressed and dragged elements until its release. Those belong to the
	// document that is going away, and the finger must not dispatch into them any more.
	for (auto& entry : secondary_touch_states)
	{
		SecondaryTouchState& state = entry.second;
		if (state.pressed_element && state.pressed_element->GetOwnerDocument() == document)
			state.pressed_element = nullptr;

		if (state.drag_element && state.drag_element->GetOwnerDocument() == document)
		{
			state.drag_element = nullptr;
			state.dragging = false;
			state.drag_proxy.reset();
		}

		// VaCuus patch #19: a target in the unloaded document takes its chain with it; the chain runs up through the document.
		if (state.drag_hover && state.drag_hover->GetOwnerDocument() == document)
		{
			state.drag_hover = nullptr;
			state.drag_hover_chain.clear();
		}
	}

	for (PointerState* state : {&mouse_pointer, &touch_pointer})
	{
		PointerScope scope(*this, *state);
		UpdateHoverChain(pointer->mouse_position);
	}
}

void Context::UnloadAllDocuments()
{
	// Unload all children.
	while (root->GetNumChildren(true) > 0)
		UnloadDocument(root->GetChild(0)->GetOwnerDocument());

	// The element lists may point to elements that are getting removed.
	pointer->active_chain.clear();
	pointer->hover_chain.clear();
	pointer->drag_hover_chain.clear();
}

void Context::EnableMouseCursor(bool enable)
{
	// The cursor is set to an invalid name so that it is forced to update in the next update loop.
	cursor_name = ":reset:";
	enable_cursor = enable;
}

void Context::ActivateTheme(const String& theme_name, bool activate)
{
	bool theme_changed = false;

	if (activate)
		theme_changed = active_themes.insert(theme_name).second;
	else
		theme_changed = (active_themes.erase(theme_name) > 0);

	if (theme_changed)
	{
		for (int i = 0; i < root->GetNumChildren(true); ++i)
		{
			if (ElementDocument* document = root->GetChild(i)->GetOwnerDocument())
				document->DirtyMediaQueries();
		}
	}
}

bool Context::IsThemeActive(const String& theme_name) const
{
	return active_themes.count(theme_name);
}

ElementDocument* Context::GetDocument(const String& id)
{
	for (int i = 0; i < root->GetNumChildren(); i++)
	{
		ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
		if (document == nullptr)
			continue;

		if (document->GetId() == id)
			return document;
	}

	return nullptr;
}

ElementDocument* Context::GetDocument(int index)
{
	Element* element = root->GetChild(index);
	if (element == nullptr)
		return nullptr;

	return element->GetOwnerDocument();
}

int Context::GetNumDocuments() const
{
	return root->GetNumChildren();
}

Element* Context::GetHoverElement()
{
	return mouse_pointer.mouse_active ? mouse_pointer.hover : touch_pointer.hover;
}

Element* Context::GetFocusElement()
{
	return focus;
}

Element* Context::GetRootElement()
{
	return root.get();
}

void Context::PullDocumentToFront(ElementDocument* document)
{
	if (document != root->GetLastChild())
	{
		// Calling RemoveChild() / AppendChild() would be cleaner, but that dirties the document's layout
		// unnecessarily, so we'll go under the hood here.
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin() + root->GetNumChildren(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

void Context::PushDocumentToBack(ElementDocument* document)
{
	if (document != root->GetFirstChild())
	{
		// See PullDocumentToFront().
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

void Context::UnfocusDocument(ElementDocument* document)
{
	auto it = std::find(document_focus_history.begin(), document_focus_history.end(), document);
	if (it != document_focus_history.end())
		document_focus_history.erase(it);

	if (!document_focus_history.empty())
		document_focus_history.back()->GetFocusLeafNode()->Focus();
}

void Context::AddEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->AddEventListener(event, listener, in_capture_phase);
}

void Context::RemoveEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->RemoveEventListener(event, listener, in_capture_phase);
}

bool Context::ProcessKeyDown(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	if (focus)
		return focus->DispatchEvent(EventId::Keydown, parameters);
	else
		return root->DispatchEvent(EventId::Keydown, parameters);
}

bool Context::ProcessKeyUp(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	if (focus)
		return focus->DispatchEvent(EventId::Keyup, parameters);
	else
		return root->DispatchEvent(EventId::Keyup, parameters);
}

bool Context::ProcessTextInput(char character)
{
	// Only the standard ASCII character set is a valid subset of UTF-8.
	if (static_cast<unsigned char>(character) > 127)
		return false;
	return ProcessTextInput(static_cast<Character>(character));
}

bool Context::ProcessTextInput(Character character)
{
	// Generate the parameters for the key event.
	String text = StringUtilities::ToUTF8(character);
	return ProcessTextInput(text);
}

bool Context::ProcessTextInput(const String& string)
{
	Element* target = (focus ? focus : root.get());

	Dictionary parameters;
	parameters["text"] = string;

	bool consumed = target->DispatchEvent(EventId::Textinput, parameters);

	return consumed;
}

bool Context::ProcessMouseMove(int x, int y, int key_modifier_state)
{
	PointerScope scope(*this, mouse_pointer);
	return ProcessPointerMove(x, y, key_modifier_state);
}

bool Context::ProcessMouseMoveIfActive(int x, int y, int key_modifier_state)
{
	PointerScope scope(*this, mouse_pointer);
	if (!pointer->mouse_active)
		return !IsPointerInteracting();
	return ProcessPointerMove(x, y, key_modifier_state);
}

bool Context::ProcessMouseButtonDown(int button_index, int key_modifier_state)
{
	PointerScope scope(*this, mouse_pointer);
	return ProcessPointerButtonDown(button_index, key_modifier_state);
}

bool Context::ProcessMouseButtonUp(int button_index, int key_modifier_state)
{
	PointerScope scope(*this, mouse_pointer);
	return ProcessPointerButtonUp(button_index, key_modifier_state);
}

bool Context::ProcessMouseWheel(Vector2f wheel_delta, int key_modifier_state)
{
	PointerScope scope(*this, mouse_pointer);
	return ProcessPointerWheel(wheel_delta, key_modifier_state);
}

bool Context::ProcessMouseLeave()
{
	PointerScope scope(*this, mouse_pointer);
	return ProcessPointerLeave();
}

bool Context::ProcessMouseCancel()
{
	PointerScope scope(*this, mouse_pointer);
	ResetActiveChain();
	const bool result = ProcessPointerButtonUp(0, 0);
	if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
		scroll_controller->Reset();
	ProcessPointerLeave();
	return result;
}

bool Context::IsMouseInteracting() const
{
	return (mouse_pointer.hover && mouse_pointer.hover != root.get()) || (mouse_pointer.active && mouse_pointer.active != root.get()) ||
		scroll_controller->GetMode() == ScrollController::Mode::Autoscroll;
}

bool Context::ProcessPointerMove(int x, int y, int key_modifier_state)
{
	// Check whether the mouse moved since the last event came through.
	Vector2i old_mouse_position = pointer->mouse_position;
	pointer->mouse_position = {x, y};
	const bool mouse_moved = (pointer->mouse_position != old_mouse_position || !pointer->mouse_active);
	pointer->mouse_active = true;

	// Update the current hover chain. This will send all necessary 'onmouseout', 'onmouseover', 'ondragout' and 'ondragover' messages.
	Dictionary parameters, drag_parameters;
	UpdateHoverChain(old_mouse_position, key_modifier_state, &parameters, &drag_parameters);

	// Dispatch any 'onmousemove' events.
	if (mouse_moved)
	{
		if (pointer->hover)
		{
			pointer->hover->DispatchEvent(EventId::Mousemove, parameters);

			if (pointer->drag_hover && pointer->drag_verbose)
				pointer->drag_hover->DispatchEvent(EventId::Dragmove, drag_parameters);
		}
	}

	return !IsPointerInteracting();
}

static Element* FindFocusElement(Element* element)
{
	ElementDocument* owner_document = element->GetOwnerDocument();
	if (!owner_document || owner_document->GetComputedValues().focus() == Style::Focus::None)
		return nullptr;

	while (element && element->GetComputedValues().focus() == Style::Focus::None)
	{
		element = element->GetParentNode();
	}

	return element;
}

bool Context::ProcessPointerButtonDown(int button_index, int key_modifier_state)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, button_index);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	bool propagate = true;

	if (button_index == 0)
	{
		Element* new_focus = pointer->hover;

		// Set the currently hovered element to focus if it isn't already the focus.
		if (pointer->hover)
		{
			new_focus = FindFocusElement(pointer->hover);
			if (new_focus && new_focus != focus)
			{
				if (!new_focus->Focus())
					return !IsPointerInteracting();
			}
		}

		// Save the just-pressed-on element as the pressed element.
		pointer->active = new_focus;

		// Call 'onmousedown' on every item in the hover chain, and copy the hover chain to the active chain.
		if (pointer->hover)
			propagate = pointer->hover->DispatchEvent(EventId::Mousedown, parameters);

		if (propagate)
		{
			// Check for a double-click on an element; if one has occured, we send the 'dblclick' event to the hover
			// element. If not, we'll start a timer to catch the next one.
			float mouse_distance_squared = float((pointer->mouse_position - pointer->last_click_mouse_position).SquaredMagnitude());
			float max_mouse_distance = DOUBLE_CLICK_MAX_DIST * density_independent_pixel_ratio;

			double click_time = GetSystemInterface()->GetElapsedTime();

			if (pointer->active == pointer->last_click_element && float(click_time - pointer->last_click_time) < DOUBLE_CLICK_TIME &&
				mouse_distance_squared < max_mouse_distance * max_mouse_distance)
			{
				if (pointer->hover)
					propagate = pointer->hover->DispatchEvent(EventId::Dblclick, parameters);

				pointer->last_click_element = nullptr;
				pointer->last_click_time = 0;
			}
			else
			{
				pointer->last_click_element = pointer->active;
				pointer->last_click_time = click_time;
			}
		}

		pointer->last_click_mouse_position = pointer->mouse_position;

		pointer->active_chain.insert(pointer->active_chain.end(), pointer->hover_chain.begin(), pointer->hover_chain.end());

		if (propagate)
		{
			// Traverse down the hierarchy of the newly focused element (if any), and see if we can begin dragging it.
			pointer->drag_started = false;
			pointer->drag = pointer->hover;
			while (pointer->drag)
			{
				Style::Drag drag_style = pointer->drag->GetComputedValues().drag();
				switch (drag_style)
				{
				case Style::Drag::None: pointer->drag = pointer->drag->GetParentNode(); continue;
				case Style::Drag::Block: pointer->drag = nullptr; continue;
				default: pointer->drag_verbose = (drag_style == Style::Drag::DragDrop || drag_style == Style::Drag::Clone);
				}

				break;
			}
		}
	}
	else
	{
		// Not the primary mouse button, so we're not doing any special processing.
		if (pointer->hover)
			propagate = pointer->hover->DispatchEvent(EventId::Mousedown, parameters);
	}

	if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
	{
		scroll_controller->Reset();
	}
	else if (button_index == 2 && pointer->hover && propagate)
	{
		Dictionary scroll_parameters;
		GenerateMouseEventParameters(scroll_parameters);
		GenerateKeyModifierEventParameters(scroll_parameters, key_modifier_state);
		scroll_parameters["autoscroll"] = true;

		// Dispatch a mouse scroll event, this gives elements an opportunity to block autoscroll from being initialized.
		if (pointer->hover->DispatchEvent(EventId::Mousescroll, scroll_parameters))
			scroll_controller->ActivateAutoscroll(pointer->hover->GetClosestScrollableContainer(), pointer->mouse_position);
	}

	return !IsPointerInteracting();
}

bool Context::ProcessPointerButtonUp(int button_index, int key_modifier_state)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, button_index);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	// We want to return the interaction state before handling the mouse up events, so that any active element that is released is considered to
	// capture the event.
	const bool result = !IsPointerInteracting();

	// Process primary click.
	if (button_index == 0)
	{
		// The elements in the new hover chain have the 'onmouseup' event called on them.
		if (pointer->hover)
			pointer->hover->DispatchEvent(EventId::Mouseup, parameters);

		// If the active element (the one that was being hovered over when the mouse button was pressed) is still being
		// hovered over, we click it.
		if (pointer->hover && pointer->active && pointer->active == FindFocusElement(pointer->hover))
		{
			pointer->active->DispatchEvent(EventId::Click, parameters);
		}

		ResetActiveChain();

		if (pointer->drag)
		{
			if (pointer->drag_started)
			{
				Dictionary drag_parameters;
				GenerateMouseEventParameters(drag_parameters);
				GenerateDragEventParameters(drag_parameters);
				GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

				if (pointer->drag_hover)
				{
					if (pointer->drag_verbose)
					{
						pointer->drag_hover->DispatchEvent(EventId::Dragdrop, drag_parameters);
						// User may have removed the element, do an extra check.
						if (pointer->drag_hover)
							pointer->drag_hover->DispatchEvent(EventId::Dragout, drag_parameters);
					}
				}

				if (pointer->drag)
					pointer->drag->DispatchEvent(EventId::Dragend, drag_parameters);

				ReleaseDragClone();
			}

			pointer->drag = nullptr;
			pointer->drag_hover = nullptr;
			pointer->drag_hover_chain.clear();

			// We may have changes under our mouse, this ensures that the hover chain is properly updated
			ProcessPointerMove(pointer->mouse_position.x, pointer->mouse_position.y, key_modifier_state);
		}
	}
	else
	{
		// Not the left mouse button, so we're not doing any special processing.
		if (pointer->hover)
			pointer->hover->DispatchEvent(EventId::Mouseup, parameters);
	}

	// If we have autoscrolled while holding the middle mouse button, release the autoscroll mode now.
	if (scroll_controller->HasAutoscrollMoved())
		scroll_controller->Reset();

	return result;
}

bool Context::ProcessMouseWheel(float wheel_delta, int key_modifier_state)
{
	return ProcessMouseWheel(Vector2f{0.f, wheel_delta}, key_modifier_state);
}

bool Context::ProcessPointerWheel(Vector2f wheel_delta, int key_modifier_state)
{
	if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
	{
		scroll_controller->Reset();
		return false;
	}
	else if (!pointer->hover)
	{
		scroll_controller->Reset();
		return true;
	}

	Dictionary scroll_parameters;
	GenerateMouseEventParameters(scroll_parameters);
	GenerateKeyModifierEventParameters(scroll_parameters, key_modifier_state);
	scroll_parameters["wheel_delta_x"] = wheel_delta.x;
	scroll_parameters["wheel_delta_y"] = wheel_delta.y;

	// Dispatch a mouse scroll event, this gives elements an opportunity to block scrolling from being performed.
	if (!pointer->hover->DispatchEvent(EventId::Mousescroll, scroll_parameters))
		return false;

	const float unit_scroll_length = UNIT_SCROLL_LENGTH * density_independent_pixel_ratio;
	const Vector2f scroll_length = wheel_delta * unit_scroll_length;
	Element* target = pointer->hover->GetClosestScrollableContainer();

	if (scroll_controller->GetMode() == ScrollController::Mode::Smoothscroll && scroll_controller->GetTarget() == target)
		scroll_controller->IncrementSmoothscrollTarget(scroll_length);
	else
		scroll_controller->ActivateSmoothscroll(target, scroll_length, ScrollBehavior::Auto);

	return target == nullptr;
}

bool Context::ProcessPointerLeave()
{
	pointer->mouse_active = false;

	// Update the hover chain. Now that 'mouse_active' is disabled this will remove the hover state from all elements.
	UpdateHoverChain(pointer->mouse_position);

	return !IsPointerInteracting();
}

bool Context::IsPointerInteracting() const
{
	return (pointer->hover && pointer->hover != root.get()) || (pointer->active && pointer->active != root.get()) || scroll_controller->GetMode() == ScrollController::Mode::Autoscroll;
}

Context::TouchState* Context::LookupTouch(TouchId identifier)
{
	auto touch_it = touch_states.find(identifier);
	return touch_it != touch_states.end() ? &touch_it->second : nullptr;
}

bool Context::ProcessTouchStart(const TouchList& touches, int key_modifier_state)
{
	PointerScope scope(*this, touch_pointer);
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchStart(touch, key_modifier_state);
	return result;
}

bool Context::ProcessTouchMove(const TouchList& touches, int key_modifier_state)
{
	PointerScope scope(*this, touch_pointer);
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchMove(touch, key_modifier_state);
	return result;
}

bool Context::ProcessTouchEnd(const TouchList& touches, int key_modifier_state)
{
	PointerScope scope(*this, touch_pointer);
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchEnd(touch, key_modifier_state);
	return result;
}

bool Context::ProcessTouchCancel(const TouchList& touches)
{
	PointerScope scope(*this, touch_pointer);
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchCancel(touch);
	return result;
}

// The element a press on `element` would drag: the closest one with `drag` other than none, or none if a `block` comes first.
// The same walk as ProcessMouseButtonDown, for the secondary fingers that do not run it.
static Element* FindDragElement(Element* element)
{
	while (element)
	{
		switch (element->GetComputedValues().drag())
		{
		case Style::Drag::None: element = element->GetParentNode(); break;
		case Style::Drag::Block: return nullptr;
		default: return element;
		}
	}
	return nullptr;
}

// The drag styles whose source gets the drop-target events (dragover, dragout, dragdrop, dragmove), as the primary's
// ProcessMouseButtonDown decides them. VaCuus patch #19 reads it at the press.
static bool IsVerboseDrag(const Element* drag_element)
{
	if (!drag_element)
		return false;

	const Style::Drag drag_style = drag_element->GetComputedValues().drag();
	return drag_style == Style::Drag::DragDrop || drag_style == Style::Drag::Clone;
}

static bool IsSelfOrDescendant(Element* element, const Element* ancestor)
{
	for (; element; element = element->GetParentNode())
	{
		if (element == ancestor)
			return true;
	}
	return false;
}

bool Context::ProcessTouchStart(const Touch& touch, int key_modifier_state)
{
	// VaCuus patch #18: a start for an id that is still a live secondary means its End was lost. End that finger as a
	// cancel, then treat the start as a fresh touch.
	if (secondary_touch_states.find(touch.identifier) != secondary_touch_states.end())
		ProcessSecondaryTouchCancel(touch.identifier);

	if (has_primary_touch && touch.identifier != primary_touch_id)
		return ProcessSecondaryTouchStart(touch, key_modifier_state);

	TouchState* state = LookupTouch(touch.identifier);
	RMLUI_ASSERTMSG(state == nullptr, "Receiving touch start event for an already started touch.");
	if (!state)
	{
		auto it_inserted = touch_states.emplace(touch.identifier, TouchState()).first;
		state = &it_inserted->second;
	}
	primary_touch_id = touch.identifier;
	has_primary_touch = true;

	state->start_position = touch.position;
	state->inertia_position = touch.position;
	state->last_position = touch.position;
	state->scrolling_start_time_x = 0;
	state->scrolling_start_time_y = 0;
	state->scrolling_last_time = GetSystemInterface()->GetElapsedTime();

	Element* touch_element = GetElementAtPoint(touch.position);
	state->scroll_container = touch_element ? touch_element->GetClosestScrollableContainer() : nullptr;

	// reset any scrolling when we touch the element
	if (state->scroll_container && scroll_controller->GetTarget() == state->scroll_container)
		scroll_controller->Reset();

	ProcessPointerMove(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y), key_modifier_state);

	// always assume touch press/release events are handled as left mouse button
	return ProcessPointerButtonDown(0, key_modifier_state);
}

bool Context::ProcessTouchMove(const Touch& touch, int key_modifier_state)
{
	if (secondary_touch_states.find(touch.identifier) != secondary_touch_states.end())
		return ProcessSecondaryTouchMove(touch, key_modifier_state);

	TouchState* state = LookupTouch(touch.identifier);
	if (!state)
		return true;

	state->scrolling_last_time = GetSystemInterface()->GetElapsedTime();

	if (state->scroll_container)
	{
		const Vector2f delta = touch.position - state->last_position;

		if (pointer->drag)
		{
			// Don't scroll and reset scrolling state when dragging any element (scrollbars and others)
			state->inertia_position = touch.position;
			state->last_position = touch.position;
			state->scrolling_start_time_x = 0;
			state->scrolling_start_time_y = 0;
		}
		else if (delta.x != 0 || delta.y != 0)
		{
			// Use instant scrolling when touch is pressed even when default scroll behavior is smooth.
			scroll_controller->InstantScrollOnTarget(state->scroll_container, -delta);

			const double current_time = GetSystemInterface()->GetElapsedTime();

			enum { Horizontal = 0, Vertical = 1 };
			for (int axis : {Horizontal, Vertical})
			{
				bool& state_scrolling_positive = (axis == Horizontal ? state->scrolling_right : state->scrolling_down);
				double& state_scrolling_start_time = (axis == Horizontal ? state->scrolling_start_time_x : state->scrolling_start_time_y);

				// Time set to 0 means no touch move events happened before and direction is unclear.
				const bool scroll_start = (state_scrolling_start_time == 0);

				// If the user changes direction, reset the start time and position.
				const bool going_positive = (delta[axis] > 0);
				if (delta[axis] != 0 && (going_positive != state_scrolling_positive || scroll_start))
				{
					state->inertia_position[axis] = touch.position[axis];
					state_scrolling_positive = going_positive;
					state_scrolling_start_time = state->scrolling_last_time;
				}
				else
				{
					// Move inertia position towards end position with a weight of e^-kt to better capture and
					// calculate velocity of the very last touch movements before touch release.
					const float elapsed_time = static_cast<float>(current_time - state_scrolling_start_time);
					const float weight = Math::Exp(-elapsed_time * TOUCH_MOVEMENT_DECAY_RATE);

					state->inertia_position[axis] = touch.position[axis] - (touch.position[axis] - state->inertia_position[axis]) * weight;
					state_scrolling_start_time = current_time - (current_time - state_scrolling_start_time) * weight;
				}
			}

			const float touch_max_distance = TOUCH_CLICK_MAX_DISTANCE * density_independent_pixel_ratio;
			const Vector2f delta_from_start = touch.position - state->start_position;
			if (delta_from_start.SquaredMagnitude() >= touch_max_distance * touch_max_distance)
				ResetActiveChain();
		}
	}

	state->last_position = touch.position;

	return ProcessPointerMove(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y), key_modifier_state);
}

bool Context::ProcessTouchEnd(const Touch& touch, int key_modifier_state)
{
	if (secondary_touch_states.find(touch.identifier) != secondary_touch_states.end())
		return ProcessSecondaryTouchEnd(touch, key_modifier_state);

	TouchState* state = LookupTouch(touch.identifier);
	if (!state)
		return true;

	if (state->scroll_container)
	{
		double current_time = GetSystemInterface()->GetElapsedTime();
		double time_since_last_move = current_time - state->scrolling_last_time;
		if (time_since_last_move < SCROLL_INERTIA_DELAY)
		{
			// apply scrolling inertia
			Vector2f delta = touch.position - state->inertia_position;

			Vector2f velocity = -delta;
			float elapsed_time_x = static_cast<float>(current_time - state->scrolling_start_time_x);
			float elapsed_time_y = static_cast<float>(current_time - state->scrolling_start_time_y);
			if (elapsed_time_x > 0)
				velocity.x /= elapsed_time_x;
			if (elapsed_time_y > 0)
				velocity.y /= elapsed_time_y;

			scroll_controller->ActivateInertia(state->scroll_container, velocity);
		}
	}

	touch_states.erase(touch.identifier);
	has_primary_touch = false;

	ProcessPointerMove(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y), key_modifier_state);

	// always assume touch press/release events are handled as left mouse button
	const bool result = ProcessPointerButtonUp(0, key_modifier_state);
	ProcessPointerLeave();
	return result;
}

bool Context::ProcessTouchCancel(const Touch& touch)
{
	if (secondary_touch_states.find(touch.identifier) != secondary_touch_states.end())
		return ProcessSecondaryTouchCancel(touch.identifier);

	TouchState* state = LookupTouch(touch.identifier);
	if (!state)
		return false;

	touch_states.erase(touch.identifier);
	has_primary_touch = false;

	// Cancellation pairs the release without turning a stolen press into a click.
	ResetActiveChain();
	const bool result = ProcessPointerButtonUp(0, 0);
	ProcessPointerLeave();
	return result;
}

bool Context::ProcessSecondaryTouchStart(const Touch& touch, int key_modifier_state)
{
	auto it_inserted = secondary_touch_states.emplace(touch.identifier, SecondaryTouchState());
	RMLUI_ASSERTMSG(it_inserted.second, "Receiving touch start event for an already started touch.");
	if (!it_inserted.second)
		return !IsPointerInteracting() && !IsSecondaryTouchInteracting();

	SecondaryTouchState& state = it_inserted.first->second;
	state.start_position = touch.position;
	state.last_position = touch.position;

	Element* touch_element = GetElementAtPoint(touch.position);
	state.pressed_element = touch_element;
	for (Element* element = touch_element; element; element = element->GetParentNode())
		state.active_chain.push_back(element);
	AcquireSecondaryActive(state.active_chain);

	// Unlike the primary, this touches neither hover nor focus nor scrolling: it only presses the element under the finger.
	Dictionary parameters;
	GenerateSecondaryTouchParameters(parameters, touch.position, touch.identifier, key_modifier_state);
	parameters["button"] = 0;
	bool propagate = true;
	if (touch_element)
		propagate = touch_element->DispatchEvent(EventId::Mousedown, parameters);

	// As for the primary, a press the page prevents starts no drag. Read the element back from the state: a mousedown
	// handler may have removed it, which OnElementDetach reports by clearing the field.
	if (propagate)
	{
		state.drag_element = FindDragElement(state.pressed_element);
		state.drag_verbose = IsVerboseDrag(state.drag_element);
	}

	return !IsPointerInteracting() && !IsSecondaryTouchInteracting();
}

bool Context::ProcessSecondaryTouchMove(const Touch& touch, int key_modifier_state)
{
	auto it = secondary_touch_states.find(touch.identifier);
	if (it == secondary_touch_states.end())
		return true;

	SecondaryTouchState& state = it->second;
	const Vector2f previous_position = state.last_position;
	state.last_position = touch.position;

	if (touch.position != previous_position)
	{
		UpdateSecondaryTouchDrag(state, touch.identifier, previous_position, touch.position, key_modifier_state);

		// Captured: the move goes to the element that was pressed, however far the finger has travelled since.
		if (state.pressed_element)
		{
			Dictionary parameters;
			GenerateSecondaryTouchParameters(parameters, touch.position, touch.identifier, key_modifier_state);
			state.pressed_element->DispatchEvent(EventId::Mousemove, parameters);
		}
	}

	return !IsPointerInteracting() && !IsSecondaryTouchInteracting();
}

bool Context::ProcessSecondaryTouchEnd(const Touch& touch, int key_modifier_state)
{
	auto it = secondary_touch_states.find(touch.identifier);
	if (it == secondary_touch_states.end())
		return true;

	// Taken before the release, as ProcessMouseButtonUp does, so the element that was pressed still captures the event.
	const bool result = !IsPointerInteracting() && !IsSecondaryTouchInteracting();

	SecondaryTouchState& state = it->second;
	const Vector2f previous_position = state.last_position;
	state.last_position = touch.position;
	if (touch.position != previous_position)
		UpdateSecondaryTouchDrag(state, touch.identifier, previous_position, touch.position, key_modifier_state);

	if (state.pressed_element)
	{
		Dictionary parameters;
		GenerateSecondaryTouchParameters(parameters, touch.position, touch.identifier, key_modifier_state);
		parameters["button"] = 0;
		state.pressed_element->DispatchEvent(EventId::Mouseup, parameters);
	}

	// Only a press that would drag is held to the click radius: drag_element is what the drag walk found at the press.
	// Elsewhere a release over the pressed element clicks, as the primary does outside scroll containers.
	const bool radius_applies = state.drag_element != nullptr;
	const bool within_radius = !radius_applies || !state.moved_beyond_click_distance;
	if (state.pressed_element && within_radius)
	{
		if (IsSelfOrDescendant(GetElementAtPoint(touch.position), state.pressed_element))
		{
			Dictionary parameters;
			GenerateSecondaryTouchParameters(parameters, touch.position, touch.identifier, key_modifier_state);
			parameters["button"] = 0;
			state.pressed_element->DispatchEvent(EventId::Click, parameters);
		}
	}

	ReleaseSecondaryActive(state.active_chain);

	DispatchSecondaryDragEnd(state, touch.position, touch.identifier, key_modifier_state);

	state.drag_proxy.reset();
	secondary_touch_states.erase(touch.identifier);
	return result;
}

bool Context::ProcessSecondaryTouchCancel(TouchId identifier)
{
	auto it = secondary_touch_states.find(identifier);
	if (it == secondary_touch_states.end())
		return true;

	const bool result = !IsPointerInteracting() && !IsSecondaryTouchInteracting();

	// A cancelled finger never clicks. Its drag ends where the finger last reported, since the cancel carries no position
	// worth trusting. Its pressed element still gets the mouseup the primary's cancel sends through ProcessMouseButtonUp,
	// so a press always pairs with a release. A drop target under it gets the drop the primary's cancel gives (VaCuus patch #19).
	SecondaryTouchState& state = it->second;

	DispatchSecondaryDragEnd(state, state.last_position, identifier, 0);

	if (state.pressed_element)
	{
		Dictionary parameters;
		GenerateSecondaryTouchParameters(parameters, state.last_position, identifier, 0);
		parameters["button"] = 0;
		state.pressed_element->DispatchEvent(EventId::Mouseup, parameters);
	}

	ReleaseSecondaryActive(state.active_chain);

	state.drag_proxy.reset();
	secondary_touch_states.erase(identifier);
	return result;
}

void Context::UpdateSecondaryTouchDrag(SecondaryTouchState& state, TouchId identifier, Vector2f previous_position, Vector2f position,
	int key_modifier_state)
{
	const float touch_max_distance = TOUCH_CLICK_MAX_DISTANCE * density_independent_pixel_ratio;
	const Vector2f delta_from_start = position - state.start_position;
	if (!state.moved_beyond_click_distance && delta_from_start.SquaredMagnitude() > touch_max_distance * touch_max_distance)
	{
		state.moved_beyond_click_distance = true;

		// The drag starts on the first move past the radius and reports the position the finger came from, as the primary's
		// dragstart does (ProcessMouseMove's UpdateHoverChain).
		if (state.drag_element)
		{
			state.dragging = true;
			Dictionary parameters;
			GenerateSecondaryTouchParameters(parameters, previous_position, identifier, key_modifier_state);
			parameters["drag_element"] = (void*)state.drag_element;
			state.drag_element->DispatchEvent(EventId::Dragstart, parameters);
			if (state.dragging && state.drag_element && state.drag_element->GetComputedValues().drag() == Style::Drag::Clone)
				state.drag_proxy = CreateDragCloneProxy(state.drag_element, state.start_position);
		}
	}

	// dragstart's handlers may have removed the source; OnElementDetach clears both fields then, so test them again.
	if (state.dragging && state.drag_element)
	{
		Dictionary parameters;
		GenerateSecondaryTouchParameters(parameters, position, identifier, key_modifier_state);
		parameters["drag_element"] = (void*)state.drag_element;
		state.drag_element->DispatchEvent(EventId::Drag, parameters);
	}

	// VaCuus patch #19: the drop targets follow the finger after its drag events, as the primary's UpdateHoverChain does.
	if (state.dragging && state.drag_element)
	{
		UpdateSecondaryDropTarget(state, identifier, position, key_modifier_state);
	}
}

void Context::UpdateSecondaryDropTarget(SecondaryTouchState& state, TouchId identifier, Vector2f position, int key_modifier_state)
{
	if (!state.drag_verbose)
		return;

	// The new chain goes into the state before any event is sent. A handler that removes an element of it reaches
	// OnElementDetach, which then clears the state's copy; a local copy would keep the stale pointer.
	ElementSet old_drag_hover_chain;
	old_drag_hover_chain.swap(state.drag_hover_chain);

	state.drag_hover = GetElementAtPoint(position, state.drag_element);
	for (Element* element = state.drag_hover; element != nullptr; element = element->GetParentNode())
		state.drag_hover_chain.insert(element);

	Dictionary parameters;
	GenerateSecondaryTouchParameters(parameters, position, identifier, key_modifier_state);
	parameters["drag_element"] = (void*)state.drag_element;

	SendEvents(old_drag_hover_chain, state.drag_hover_chain, EventId::Dragout, parameters);
	SendEvents(state.drag_hover_chain, old_drag_hover_chain, EventId::Dragover, parameters);

	// A dragover handler may have removed the source or the target; either clears its field, so test both again.
	if (state.dragging && state.drag_element && state.drag_hover)
		state.drag_hover->DispatchEvent(EventId::Dragmove, parameters);
}

void Context::DispatchSecondaryDragEnd(SecondaryTouchState& state, Vector2f position, TouchId identifier, int key_modifier_state)
{
	if (!state.dragging || !state.drag_element)
		return;

	// The primary's order: the drop on the target, the target's dragout, then the source's dragend.
	if (state.drag_verbose && state.drag_hover)
	{
		Dictionary parameters;
		GenerateSecondaryTouchParameters(parameters, position, identifier, key_modifier_state);
		parameters["drag_element"] = (void*)state.drag_element;

		state.drag_hover->DispatchEvent(EventId::Dragdrop, parameters);
		// The drop may have removed the target; OnElementDetach has cleared the field then.
		if (state.drag_hover)
			state.drag_hover->DispatchEvent(EventId::Dragout, parameters);
	}

	// The drop may have removed the source, which clears dragging, so test again before the dragend.
	if (state.dragging && state.drag_element)
	{
		Dictionary parameters;
		GenerateSecondaryTouchParameters(parameters, position, identifier, key_modifier_state);
		parameters["drag_element"] = (void*)state.drag_element;
		state.drag_element->DispatchEvent(EventId::Dragend, parameters);
	}
}

bool Context::IsSecondaryTouchInteracting() const
{
	for (const auto& entry : secondary_touch_states)
	{
		const Element* pressed_element = entry.second.pressed_element;
		if (pressed_element && pressed_element != root.get())
			return true;
	}
	return false;
}

void Context::AcquireSecondaryActive(const ElementList& chain)
{
	for (Element* element : chain)
	{
		++secondary_active_counts[element];
		element->SetPseudoClass("active", true);
	}
}

void Context::ReleaseSecondaryActive(const ElementList& chain)
{
	for (Element* element : chain)
	{
		auto it = secondary_active_counts.find(element);
		// OnElementDetach forgot this element: it is gone, so there is nothing left to clear.
		if (it == secondary_active_counts.end())
			continue;

		if (--it->second > 0)
			continue;

		secondary_active_counts.erase(it);

		// The primary's chain may still hold :active on this element. ResetActiveChain clears it when the primary releases.
		const bool held_by_primary =
			std::find(mouse_pointer.active_chain.begin(), mouse_pointer.active_chain.end(), element) != mouse_pointer.active_chain.end() ||
			std::find(touch_pointer.active_chain.begin(), touch_pointer.active_chain.end(), element) != touch_pointer.active_chain.end();
		if (!held_by_primary)
			element->SetPseudoClass("active", false);
	}
}

void Context::GenerateSecondaryTouchParameters(Dictionary& parameters, Vector2f position, TouchId identifier, int key_modifier_state)
{
	parameters["mouse_x"] = static_cast<int>(position.x);
	parameters["mouse_y"] = static_cast<int>(position.y);
	parameters["touch_id"] = static_cast<uint64_t>(identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);
}

void Context::SetDefaultScrollBehavior(ScrollBehavior scroll_behavior, float speed_factor)
{
	scroll_controller->SetDefaultScrollBehavior(scroll_behavior, speed_factor);
}

RenderManager& Context::GetRenderManager()
{
	return *render_manager;
}

TextInputHandler* Context::GetTextInputHandler() const
{
	return text_input_handler;
}

void Context::SetInstancer(ContextInstancer* _instancer)
{
	RMLUI_ASSERT(instancer == nullptr);
	instancer = _instancer;
}

DataModelConstructor Context::CreateDataModel(const String& name, DataTypeRegister* data_type_register, bool allow_missing_variables)
{
	if (!data_type_register)
	{
		if (!default_data_type_register)
			default_data_type_register = MakeUnique<DataTypeRegister>();
		data_type_register = default_data_type_register.get();
	}

	auto result = data_models.emplace(name, MakeUnique<DataModel>(data_type_register, allow_missing_variables));
	bool inserted = result.second;
	if (inserted)
	{
		DataModel* model = result.first->second.get();
		PluginRegistry::NotifyDataModelCreate(this, name);
		return DataModelConstructor(model);
	}

	Log::Message(Log::LT_ERROR, "Data model name '%s' already exists.", name.c_str());
	return DataModelConstructor();
}

DataModelConstructor Context::GetDataModel(const String& name)
{
	if (DataModel* model = GetDataModelPtr(name))
		return DataModelConstructor(model);

	Log::Message(Log::LT_ERROR, "Data model name '%s' could not be found.", name.c_str());
	return DataModelConstructor();
}

UnorderedMap<String, DataModelConstructor> Context::GetDataModels() const
{
	UnorderedMap<String, DataModelConstructor> result;
	result.reserve(data_models.size());
	for (const auto& pair : data_models)
		result.emplace(pair.first, DataModelConstructor(pair.second.get()));
	return result;
}

bool Context::RemoveDataModel(const String& name)
{
	auto it = data_models.find(name);
	if (it == data_models.end())
		return false;

	DataModel* model = it->second.get();
	ElementList elements = model->GetAttachedModelRootElements();

	for (Element* element : elements)
		element->SetDataModel(nullptr);

	PluginRegistry::NotifyDataModelDestroy(this, it->first);

	data_models.erase(it);

	return true;
}

void Context::OnPointerElementDetach(PointerState& state, Element* element)
{
	PointerScope scope(*this, state);
	auto it_hover = pointer->hover_chain.find(element);
	if (it_hover != pointer->hover_chain.end())
	{
		Dictionary parameters;
		GenerateMouseEventParameters(parameters, -1);
		element->DispatchEvent(EventId::Mouseout, parameters);

		pointer->hover_chain.erase(it_hover);

		if (pointer->hover == element)
			pointer->hover = nullptr;
	}

	auto it_active = std::find(pointer->active_chain.begin(), pointer->active_chain.end(), element);
	if (it_active != pointer->active_chain.end())
	{
		pointer->active_chain.erase(it_active);

		if (pointer->active == element)
			pointer->active = nullptr;
	}

	if (pointer->drag)
	{
		auto it = pointer->drag_hover_chain.find(element);
		if (it != pointer->drag_hover_chain.end())
		{
			pointer->drag_hover_chain.erase(it);

			if (pointer->drag_hover == element)
				pointer->drag_hover = nullptr;
		}

		if (pointer->drag == element)
		{
			// The dragged element is being removed, silently cancel the drag operation
			if (pointer->drag_started)
				ReleaseDragClone();

			pointer->drag = nullptr;
			pointer->drag_hover = nullptr;
			pointer->drag_hover_chain.clear();
		}
	}
}

void Context::OnElementDetach(Element* element)
{
	for (PointerState* state : {&mouse_pointer, &touch_pointer})
		OnPointerElementDetach(*state, element);

	// VaCuus patch #18: a secondary finger forgets the element too. Its :active count goes, and every chain it is held in
	// drops the element, so that a later release cannot write through the stale address.
	secondary_active_counts.erase(element);
	for (auto& entry : secondary_touch_states)
	{
		SecondaryTouchState& state = entry.second;
		if (state.pressed_element == element)
			state.pressed_element = nullptr;

		// Silently cancelled, as the primary's drag is below: no dragend, and no further drag events.
		if (state.drag_element == element)
		{
			state.drag_element = nullptr;
			state.dragging = false;
			state.drag_proxy.reset();
			state.drag_hover = nullptr;
			state.drag_hover_chain.clear();
		}

		// VaCuus patch #19: a removed drop target leaves the field and the chain, as the source does above.
		if (state.drag_hover == element)
			state.drag_hover = nullptr;
		auto it_drag_hover = state.drag_hover_chain.find(element);
		if (it_drag_hover != state.drag_hover_chain.end())
			state.drag_hover_chain.erase(it_drag_hover);

		state.active_chain.erase(std::remove(state.active_chain.begin(), state.active_chain.end(), element), state.active_chain.end());
	}

	// Focus normally cleared and set by parent during Element::RemoveChild.
	// However, there are some exceptions, such as when an there are multiple
	// ElementDocuments in the hierarchy above the current element.
	if (element == focus)
		focus = nullptr;

	// If the element is a document lower down in the hierarchy, we may need to remove it from the focus history.
	if (element->GetOwnerDocument() == element)
	{
		auto it = std::find(document_focus_history.begin(), document_focus_history.end(), element);
		if (it != document_focus_history.end())
			document_focus_history.erase(it);
	}

	if (scroll_controller->GetTarget() == element)
		scroll_controller->Reset();

	// Clear TouchState if we're touching element
	for (auto touch_it = touch_states.begin(); touch_it != touch_states.end();)
	{
		if (touch_it->second.scroll_container == element)
		{
			// VaCuus patch #18: the primary slot is free again, as after its End or Cancel. Otherwise the stale flag routes
			// every later finger to the secondary path.
			if (has_primary_touch && touch_it->first == primary_touch_id)
				has_primary_touch = false;

			touch_it = touch_states.erase(touch_it);
		}
		else
			++touch_it;
	}
}

bool Context::OnFocusChange(Element* new_focus, bool focus_visible)
{
	RMLUI_ASSERT(new_focus);

	ElementSet old_chain;
	ElementSet new_chain;

	Element* old_focus = focus;
	ElementDocument* old_document = old_focus ? old_focus->GetOwnerDocument() : nullptr;
	ElementDocument* new_document = new_focus->GetOwnerDocument();

	// If the current focus is modal and the new focus is cannot receive focus from modal, deny the request.
	if (old_document && old_document->IsModal() && (!new_document || !(new_document->IsModal() || new_document->IsFocusableFromModal())))
		return false;

	// If the document of the new focus has been closed, deny the request.
	if (std::find_if(unloaded_documents.begin(), unloaded_documents.end(),
			[&](const auto& unloaded_document) { return unloaded_document.get() == new_document; }) != unloaded_documents.end())
	{
		return false;
	}

	// Build the old chains
	Element* element = old_focus;
	while (element)
	{
		old_chain.insert(element);
		element = element->GetParentNode();
	}

	// Build the new chain
	element = new_focus;
	while (element)
	{
		new_chain.insert(element);
		element = element->GetParentNode();
	}

	// Send out blur/focus events.
	Dictionary parameters;
	SendEvents(old_chain, new_chain, EventId::Blur, parameters);

	if (focus_visible)
		parameters["focus_visible"] = true;

	SendEvents(new_chain, old_chain, EventId::Focus, parameters);

	focus = new_focus;

	// Raise the element's document to the front, if desired.
	ElementDocument* document = focus->GetOwnerDocument();
	if (document != nullptr)
	{
		Style::ZIndex z_index_property = document->GetComputedValues().z_index();
		if (z_index_property.type == Style::ZIndex::Auto)
			document->PullToFront();
	}

	// Update the focus history
	if (old_document != new_document)
	{
		// If documents have changed, add the new document to the end of the history
		ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), new_document);
		if (itr != document_focus_history.end())
			document_focus_history.erase(itr);

		if (new_document != nullptr)
			document_focus_history.push_back(new_document);
	}

	return true;
}

void Context::GenerateClickEvent(Element* element)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, 0);

	element->DispatchEvent(EventId::Click, parameters);
}

void Context::UpdateHoverChain(Vector2i old_mouse_position, int key_modifier_state, Dictionary* out_parameters, Dictionary* out_drag_parameters)
{
	const Vector2f position(pointer->mouse_position);

	Dictionary local_parameters, local_drag_parameters;
	Dictionary& parameters = out_parameters ? *out_parameters : local_parameters;
	Dictionary& drag_parameters = out_drag_parameters ? *out_drag_parameters : local_drag_parameters;

	// Generate the parameters for the mouse events (there could be a few!).
	GenerateMouseEventParameters(parameters);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	GenerateMouseEventParameters(drag_parameters);
	GenerateDragEventParameters(drag_parameters);
	GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

	// Send out drag events.
	if (pointer->drag)
	{
		if (pointer->mouse_position != old_mouse_position)
		{
			if (!pointer->drag_started)
			{
				Dictionary drag_start_parameters = drag_parameters;
				drag_start_parameters["mouse_x"] = old_mouse_position.x;
				drag_start_parameters["mouse_y"] = old_mouse_position.y;
				pointer->drag->DispatchEvent(EventId::Dragstart, drag_start_parameters);
				pointer->drag_started = true;

				if (pointer->drag && pointer->drag->GetComputedValues().drag() == Style::Drag::Clone)
				{
					// Clone the element and attach it to the mouse cursor.
					CreateDragClone(pointer->drag);
				}
			}

			if (pointer->drag)
				pointer->drag->DispatchEvent(EventId::Drag, drag_parameters);
		}
	}

	pointer->hover = pointer->mouse_active ? GetElementAtPoint(position) : nullptr;

	if (enable_cursor && pointer == &mouse_pointer)
	{
		String new_cursor_name;

		if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
			new_cursor_name = scroll_controller->GetAutoscrollCursor(pointer->mouse_position, density_independent_pixel_ratio);
		else if (pointer->drag)
			new_cursor_name = pointer->drag->GetComputedValues().cursor();
		else if (pointer->hover)
			new_cursor_name = pointer->hover->GetComputedValues().cursor();

		if (new_cursor_name != cursor_name)
		{
			GetSystemInterface()->SetMouseCursor(new_cursor_name);
			cursor_name = new_cursor_name;
		}
	}

	// Build the new hover chain.
	ElementSet new_hover_chain;
	Element* element = pointer->hover;
	while (element != nullptr)
	{
		new_hover_chain.insert(element);
		element = element->GetParentNode();
	}

	// Send mouseout / mouseover events.
	SendEvents(pointer->hover_chain, new_hover_chain, EventId::Mouseout, parameters);
	SendEvents(new_hover_chain, pointer->hover_chain, EventId::Mouseover, parameters);

	// Send out drag events.
	if (pointer->drag && pointer->mouse_active)
	{
		pointer->drag_hover = GetElementAtPoint(position, pointer->drag);

		ElementSet new_drag_hover_chain;
		element = pointer->drag_hover;
		while (element != nullptr)
		{
			new_drag_hover_chain.insert(element);
			element = element->GetParentNode();
		}

		if (pointer->drag_started && pointer->drag_verbose)
		{
			// Send out ondragover and ondragout events as appropriate.
			SendEvents(pointer->drag_hover_chain, new_drag_hover_chain, EventId::Dragout, drag_parameters);
			SendEvents(new_drag_hover_chain, pointer->drag_hover_chain, EventId::Dragover, drag_parameters);
		}

		pointer->drag_hover_chain.swap(new_drag_hover_chain);
	}

	// Swap the new chain in.
	pointer->hover_chain.swap(new_hover_chain);
	const PointerState& other = (pointer == &mouse_pointer ? touch_pointer : mouse_pointer);
	Vector<ObserverPtr<Element>> held_hover;
	for (Element* held : other.hover_chain)
		held_hover.emplace_back(held->GetObserverPtr());
	for (auto& held : held_hover)
	{
		if (held)
			held->SetPseudoClass("hover", true);
	}
}

void Context::ResetActiveChain()
{
	// Unset the 'active' pseudo-class on all the elements in the active chain; because they may not necessarily
	// have had 'onmouseup' called on them, we can't guarantee this has happened already.
	for (Element* element : pointer->active_chain)
	{
		// A secondary finger that still holds :active on this element keeps it (VaCuus patch #18).
		const PointerState& other = (pointer == &mouse_pointer ? touch_pointer : mouse_pointer);
		if (secondary_active_counts.find(element) == secondary_active_counts.end() &&
			std::find(other.active_chain.begin(), other.active_chain.end(), element) == other.active_chain.end())
			element->SetPseudoClass("active", false);
	}

	pointer->active_chain.clear();
	pointer->active = nullptr;
}

Element* Context::GetElementAtPoint(Vector2f point, const Element* ignore_element, Element* element) const
{
	if (!element)
	{
		if (ignore_element == root.get())
			return nullptr;

		element = root.get();
	}

	bool is_modal = false;
	ElementDocument* focus_document = nullptr;

	// If we have modal focus, only check down documents that can receive focus from modals.
	if (element == root.get() && focus)
	{
		focus_document = focus->GetOwnerDocument();
		if (focus_document && focus_document->IsModal())
			is_modal = true;
	}

	// Check any elements within our stacking context. We want to return the lowest-down element
	// that is under the cursor.
	if (element->local_stacking_context)
	{
		if (element->stacking_context_dirty)
			element->BuildLocalStackingContext();

		for (int i = (int)element->stacking_context.size() - 1; i >= 0; --i)
		{
			Element* stacking_child = element->stacking_context[i];
			if (ignore_element)
			{
				// Check if the element is a descendant of the element we're ignoring.
				Element* element_hierarchy = stacking_child;
				while (element_hierarchy)
				{
					if (element_hierarchy == ignore_element)
						break;

					element_hierarchy = element_hierarchy->GetParentNode();
				}

				if (element_hierarchy)
					continue;
			}

			if (is_modal)
			{
				ElementDocument* child_document = stacking_child->GetOwnerDocument();
				if (!child_document || !(child_document == focus_document || child_document->IsFocusableFromModal()))
					continue;
			}

			Element* child_element = GetElementAtPoint(point, ignore_element, stacking_child);
			if (child_element)
				return child_element;
		}
	}

	// Ignore elements whose pointer events are disabled.
	if (element->GetComputedValues().pointer_events() == Style::PointerEvents::None)
		return nullptr;

	// Projection may fail if we have a singular transformation matrix.
	bool projection_result = element->Project(point);

	// Check if the point is actually within this element.
	bool within_element = (projection_result && element->IsPointWithinElement(point));
	if (within_element)
	{
		// The element may have been clipped out of view if it overflows an ancestor, so check its clipping region.
		Rectanglei clip_region;
		if (ElementUtilities::GetClippingRegion(element, clip_region))
			within_element = clip_region.Contains(Vector2i(point));
	}

	if (within_element)
		return element;

	return nullptr;
}

ElementPtr Context::CreateDragCloneProxy(Element* element, Vector2f position)
{
	ElementPtr clone = element->Clone();
	if (!clone)
	{
		Log::Message(Log::LT_ERROR, "Unable to duplicate drag clone.");
		return nullptr;
	}

	ElementPtr proxy = CreateCursorProxy();
	if (ElementDocument* document = element->GetOwnerDocument())
	{
		static_cast<ElementDocument&>(*proxy).source_url = document->GetSourceURL();
		static_cast<ElementDocument&>(*proxy).SetStyleSheetContainer(document->style_sheet_container);
	}

	Element* clone_element = clone.get();
	proxy->AppendChild(std::move(clone));

	const Vector2f absolute_pos = element->GetAbsoluteOffset(BoxArea::Border);
	if (Element* parent = element->GetParentNode())
		parent->Project(position);

	clone_element->SetProperty(PropertyId::Position, Property(Style::Position::Absolute));
	clone_element->SetProperty(PropertyId::Left, Property(absolute_pos.x - position.x, Unit::PX));
	clone_element->SetProperty(PropertyId::Top, Property(absolute_pos.y - position.y, Unit::PX));
	clone_element->SetProperty(PropertyId::MarginLeft, Property(0.f, Unit::PX));
	clone_element->SetProperty(PropertyId::MarginTop, Property(0.f, Unit::PX));
	clone_element->SetPseudoClass("drag", true);
	return proxy;
}

void Context::RenderDragCloneProxy(Element* proxy, Vector2f position)
{
	if (!proxy)
		return;

	static_cast<ElementDocument&>(*proxy).UpdateDocument();
	proxy->SetOffset(Vector2f(Math::Clamp(position.x, 0.f, float(dimensions.x)), Math::Clamp(position.y, 0.f, float(dimensions.y))), nullptr);
	proxy->Render();
}

void Context::CreateDragClone(Element* element)
{
	ReleaseDragClone();
	pointer->cursor_proxy = CreateDragCloneProxy(element, Vector2f(pointer->mouse_position));
}

void Context::ReleaseDragClone()
{
	pointer->cursor_proxy.reset();
}

void Context::PerformSmoothscrollOnTarget(Element* target, Vector2f delta_offset, ScrollBehavior scroll_behavior)
{
	scroll_controller->ActivateSmoothscroll(target, delta_offset, scroll_behavior);
}

DataModel* Context::GetDataModelPtr(const String& name) const
{
	auto it = data_models.find(name);
	if (it != data_models.end())
		return it->second.get();
	return nullptr;
}

void Context::GenerateKeyEventParameters(Dictionary& parameters, Input::KeyIdentifier key_identifier)
{
	parameters["key_identifier"] = (int)key_identifier;
}

void Context::GenerateMouseEventParameters(Dictionary& parameters, int button_index)
{
	parameters.reserve(3);
	parameters["mouse_x"] = pointer->mouse_position.x;
	parameters["mouse_y"] = pointer->mouse_position.y;
	if (pointer == &touch_pointer)
		parameters["touch_id"] = static_cast<uint64_t>(primary_touch_id);
	if (button_index >= 0)
		parameters["button"] = button_index;
}

void Context::GenerateKeyModifierEventParameters(Dictionary& parameters, int key_modifier_state)
{
	static const String property_names[] = {"ctrl_key", "shift_key", "alt_key", "meta_key", "caps_lock_key", "num_lock_key", "scroll_lock_key"};

	for (int i = 0; i < 7; i++)
		parameters[property_names[i]] = (int)((key_modifier_state & (1 << i)) > 0);
}

void Context::GenerateDragEventParameters(Dictionary& parameters)
{
	parameters["drag_element"] = (void*)pointer->drag;
}

void Context::ReleaseUnloadedDocuments()
{
	if (!unloaded_documents.empty())
	{
		OwnedElementList documents = std::move(unloaded_documents);
		unloaded_documents.clear();

		// Clear the deleted list.
		for (size_t i = 0; i < documents.size(); ++i)
			documents[i]->GetEventDispatcher()->DetachAllEvents();
		documents.clear();
	}
}

using ElementObserverList = Vector<ObserverPtr<Element>>;

class ElementObserverListBackInserter {
public:
	using iterator_category = std::output_iterator_tag;
	using value_type = void;
	using difference_type = void;
	using pointer = void;
	using reference = void;
	using container_type = ElementObserverList;

	ElementObserverListBackInserter(ElementObserverList& elements) : elements(&elements) {}
	ElementObserverListBackInserter& operator=(Element* element)
	{
		elements->push_back(element->GetObserverPtr());
		return *this;
	}
	ElementObserverListBackInserter& operator*() { return *this; }
	ElementObserverListBackInserter& operator++() { return *this; }
	ElementObserverListBackInserter& operator++(int) { return *this; }

private:
	ElementObserverList* elements;
};

void Context::SendEvents(const ElementSet& old_items, const ElementSet& new_items, EventId id, const Dictionary& parameters)
{
	// We put our elements in observer pointers in case some of them are deleted during dispatch.
	ElementObserverList elements;
	std::set_difference(old_items.begin(), old_items.end(), new_items.begin(), new_items.end(), ElementObserverListBackInserter(elements));
	for (auto& element : elements)
	{
		if (element)
			element->DispatchEvent(id, parameters);
	}
}

void Context::Release()
{
	if (instancer)
	{
		instancer->ReleaseContext(this);
	}
}

void Context::SetDocumentsBaseTag(const String& tag)
{
	documents_base_tag = tag;
}

const String& Context::GetDocumentsBaseTag()
{
	return documents_base_tag;
}

void Context::RequestNextUpdate(double delay)
{
	RMLUI_ASSERT(delay >= 0.0);
	next_update_timeout = Math::Min(next_update_timeout, delay);
}

double Context::GetNextUpdateDelay() const
{
	return next_update_timeout;
}

} // namespace Rml
