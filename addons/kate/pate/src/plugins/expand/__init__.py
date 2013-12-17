# -*- coding: utf-8 -*-
# This file is part of Pate, Kate' Python scripting plugin.
#
# Copyright (C) 2006 Paul Giannaros <paul@giannaros.org>
# Copyright (C) 2013 Alex Turbov <i.zaufi@gmail.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) version 3.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public License
# along with this library; see the file COPYING.LIB.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA 02110-1301, USA.

'''User-defined text expansions.


Each text expansion is a simple function which must return a string.
This string will be inserted into a document by the expandAtCursor action.
For example if you have a function "foo" in then all.expand file
which is defined as:

def foo:
  return 'Hello from foo!'

after typing "foo", the action will replace "foo" with "Hello from foo!".
The expansion function may have parameters as well. For example, the
text_x-c++src.expand file contains the "cl" function which creates a class
(or class template). This will expand "cl(test)" into:

/**
 * \\brief Class \c test
 */
class test
{
public:
    /// Default constructor
    explicit test()
    {
    }
    /// Destructor
    virtual ~test()
    {
    }
};

but "cl(test, T1, T2, T3)" will expand to:

/**
 * \\brief Class \c test
 */
template <typename T1, typename T2, typename T3>
class test
{
public:
    /// Default constructor
    explicit test()
    {
    }
    /// Destructor
    virtual ~test()
    {
    }
};
'''

from PyKDE4.kdecore import i18nc

from .udf import *

from libkatepate.autocomplete import AbstractCodeCompletionModel

__expands_completion_model = None


@kate.action
def expandAtCursorAction():
    expandAtCursor()


class ExpandsCompletionModel(AbstractCodeCompletionModel):
    TITLE_AUTOCOMPLETION = i18nc('@label:listbox', 'Expands Available')
    GROUP_POSITION = AbstractCodeCompletionModel.GroupPosition.GLOBAL

    def completionInvoked(self, view, word, invocationType):
        self.reset()
        # NOTE Do not allow automatic popup cuz most of expanders are short
        # and it will annoying when typing code...
        if invocationType == 0:
            return

        expansions = getExpansionsFor(view.document().mimeType())
        for exp, fn_tuple in expansions.items():
            # Try to get a function description (very first line)
            if hasattr(fn_tuple[0], '__description__'):
                description = fn_tuple[0].__description__.strip()
            else:
                description = None
            if hasattr(fn_tuple[0], '__details__'):
                details_text = fn_tuple[0].__details__.strip()
            else:
                details_text = None

            # Get function parameters
            fp = inspect.getargspec(fn_tuple[0])
            args = fp[0]
            params=''
            if len(args) != 0:
                params = ', '.join(args)
            if fp[1] is not None:
                if len(params):
                    params += ', '
                params += '[{}]'.format(fp[1])
            # Append to result completions list
            self.resultList.append(
                self.createItemAutoComplete(
                    text=exp
                  , description=description
                  , details = details_text
                  , args='({})'.format(params)
                  )
              )

    def reset(self):
        self.resultList = []


def _reset(*args, **kwargs):
    global __expands_completion_model
    if __expands_completion_model is not None:
        __expands_completion_model.reset()


@kate.init
def on_load():
    global __expands_completion_model
    assert(__expands_completion_model is None)
    __expands_completion_model = ExpandsCompletionModel(kate.application)
    __expands_completion_model.modelReset.connect(_reset)
    # Set completion model for all already existed views
    # (cuz the plugin can be loaded in the middle of editing session)
    for doc in kate.documentManager.documents():
        for view in doc.views():
            cci = view.codeCompletionInterface()
            cci.registerCompletionModel(__expands_completion_model)


@kate.unload
def on_unoad():
    global __expands_completion_model
    assert(__expands_completion_model is not None)
    for doc in kate.documentManager.documents():
        for view in doc.views():
            cci = view.codeCompletionInterface()
            cci.unregisterCompletionModel(__expands_completion_model)
    __expands_completion_model = None


@kate.viewCreated
def createSignalAutocompleteExpands(view):
    global __expands_completion_model
    if view:
        cci = view.codeCompletionInterface()
        cci.registerCompletionModel(__expands_completion_model)


def jinja(template):
    ''' Decorator to tell to the expand engine that decorated function
        wants to use jinja2 template to render, instead of 'legacy'
        (document.insertText()) way...
    '''
    def _decorator(func):
        func.template = template
        return func
    return _decorator


def postprocess(func):
    ''' Decorator to tell to the expand engine that decorated function
        wants to use TemplateInterface2 to insert text, so user may
        tune a rendered template...
    '''
    func.use_template_iface = True
    return func


def description(text):
    def _decorator(func):
        setattr(func, '__description__', text)
        return func
    return _decorator


def details(text):
    def _decorator(func):
        setattr(func, '__details__', text)
        return func
    return _decorator
