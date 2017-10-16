# Copyright 2017 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Crome OS Configuration access library.

Provides build-time access to the master configuration on the host. It is used
for reading from the master configuration. Consider using cros_config_host.py
for CLI access to this library.
"""

from __future__ import print_function

from collections import namedtuple, OrderedDict

import fdt

TouchFile = namedtuple('TouchFile', ['firmware', 'symlink'])


class CrosConfig(object):
  """The ChromeOS Configuration API for the host.

  CrosConfig is the top level API for accessing ChromeOS Configs from the host.

  Properties:
    models: All models in the CrosConfig tree, in the form of a dictionary:
            <model name: string, model: CrosConfig.Model>
    phandle_to_node: Map of phandles to the assocated CrosConfig.Node:
        key: Integer phandle value (>= 1)
        value: Associated CrosConfig.Node object
  """
  def __init__(self, infile):
    self._fdt = fdt.Fdt(infile)
    self._fdt.Scan()
    self.models = OrderedDict(
        (n.name, CrosConfig.Model(self, n))
        for n in self._fdt.GetNode('/chromeos/models').subnodes)
    self.phandle_to_node = dict(
        (phandle, CrosConfig.Node(self, fdt_node))
        for phandle, fdt_node in self._fdt.phandle_to_node.iteritems())

  def GetTouchFirmwareFiles(self):
    """Get a list of unique touch firmware files for all models

    Returns:
      List of TouchFile objects representing all the touch firmware referenced
      by all models
    """
    file_set = set()
    for model in self.models.values():
      for files in model.GetTouchFirmwareFiles().values():
        file_set.add(files)

    return sorted(file_set, key=lambda files: files.firmware)

  class Node(object):
    """Represents a single node in the CrosConfig tree, including Model.

    A node can have several subnodes nodes, as well as several properties. Both
    can be accessed via .subnodes and .properties respectively. A few helpers
    are also provided to make node traversal a little easier.

    Properties:
      name: The name of the this node.
      subnodes: Child nodes, in the form of a dictionary:
                <node name: string, child node: CrosConfig.Node>
      properties: All properties attached to this node in the form of a
                  dictionary: <name: string, property: CrosConfig.Property>
    """
    def __init__(self, cros_config, fdt_node):
      self.cros_config = cros_config
      self._fdt_node = fdt_node
      self.name = fdt_node.name
      self.subnodes = OrderedDict((n.name, CrosConfig.Node(cros_config, n))
                                  for n in fdt_node.subnodes)
      self.properties = OrderedDict((n, CrosConfig.Property(p))
                                    for n, p in fdt_node.props.iteritems())

    def ChildNodeFromPath(self, relative_path):
      """Returns the CrosConfig.Node at the relative path.

      This method is useful for accessing a nested child object at a relative
      path from a Node (or Model). The path must be separated with '/'
      delimiters. Return None if the path is invalid.

      Args:
        relative_path: A relative path string separated by '/'.

      Returns:
        A CrosConfig.Node at the path, or None if it doesn't exist.
      """
      if not relative_path:
        return self
      path_parts = [path for path in relative_path.split('/') if path]
      if not path_parts:
        return self
      part = path_parts[0]
      if part in self.subnodes:
        sub_node = self.subnodes[part]

      # Handle a 'shares' property, which means we can grab nodes / properties
      # from the associated node.
      elif 'shares' in self.properties:
        shared = self.FollowPhandle('shares')
        if part in shared.subnodes:
          sub_node = shared.subnodes[part]
      else:
        return None
      return sub_node.ChildNodeFromPath('/'.join(path_parts[1:]))

    def ChildPropertyFromPath(self, relative_path, property_name):
      child_node = self.ChildNodeFromPath(relative_path)
      if not child_node:
        return None
      prop = child_node.properties.get(property_name)
      if not prop and 'shares' in child_node.properties:
        shared = child_node.FollowPhandle('shares')
        prop = shared.properties.get(property_name)
      return prop

    def FollowPhandle(self, prop_name):
      """Follow a property's phandle

      Args:
        prop_name: Property name to check

      Returns:
        Node that the property's phandle points to, or None if none
      """
      prop = self.properties.get(prop_name)
      if not prop:
        return None
      return self.cros_config.phandle_to_node[prop.GetPhandle()]

    def GetMergedProperties(self, phandle_prop):
      """Obtain properties in two nodes linked by a phandle

      This is used to create a dict of the properties in a main node along with
      those found in a linked node. The link is provided by a property in the
      main node containing a single phandle pointing to the linked node.

      The result is a dict that combines both nodes' properties, with the
      linked node filling in anything missing. The main node's properties take
      precedence.

      Phandle properties and 'reg' properties are not included.

      Args:
        phandle_prop: Name of the phandle property to follow

      Returns:
        dict containing property names and values from both nodes:
          key: property name
          value: property value
      """
      # First get all the property keys/values from the current node
      props = OrderedDict((prop.name, prop.value)
                          for prop in self.properties.values()
                          if prop.name not in [phandle_prop, 'reg'])

      # Follow the phandle and add any new ones we find
      phandle_node = self.FollowPhandle(phandle_prop)
      if phandle_node:
        for name, prop in phandle_node.properties.iteritems():
          if name not in props and not name.endswith('phandle'):
            props[name] = prop.value
      return props

  class Model(Node):
    """Represents a ChromeOS Configuration Model.

    A CrosConfig.Model is a subclass of CrosConfig.Node, meaning it can be
    traversed in the same manner. It also exposes helper functions
    specific to ChromeOS Config models.
    """
    def __init__(self, cros_config, fdt_node):
      super(CrosConfig.Model, self).__init__(cros_config, fdt_node)

    def GetFirmwareUris(self):
      """Returns a list of (string) firmware URIs.

      Generates and returns a list of firmeware URIs for this model. These URIs
      can be used to pull down remote firmware packages.

      Returns:
        A list of (string) full firmware URIs, or an empty list on failure.
      """
      firmware = self.ChildNodeFromPath('/firmware')
      if not firmware:
        return []
      props = firmware.GetMergedProperties('shares')

      if 'bcs-overlay' not in props:
        return []
      # Strip "overlay-" from bcs_overlay
      bcs_overlay = props['bcs-overlay'][8:]
      valid_images = [p for n, p in props.iteritems()
                      if n.endswith('-image') and p.startswith('bcs://')]
      # Strip "bcs://" from bcs_from images (to get the file names only)
      file_names = [p[6:] for p in valid_images]
      uri_format = ('gs://chromeos-binaries/HOME/bcs-{bcs}/overlay-{bcs}/'
                    'chromeos-base/chromeos-firmware-{model}/{fname}')
      return [uri_format.format(bcs=bcs_overlay, model=self.name, fname=fname)
              for fname in file_names]

    @staticmethod
    def GetTouchFilename(node_path, props, fname_prop):
      """Create a touch filename based on the given template and properties

      Args:
        node_path: Path of the node generating this filename (for error
            reporting only)
        props: Dict of properties which can be used in the template:
            key: Variable name
            value: Value of that variable
        fname_prop: Name of property containing the template
      """
      template = props[fname_prop].replace('$', '')
      try:
        return template.format(props, **props)
      except KeyError as e:
        raise ValueError(("node '%s': Format string '%s' has properties '%s' " +
                          "but lacks '%s'") %
                         (node_path, template, props.keys(), e.message))

    def GetTouchFirmwareFiles(self):
      """Get a list of unique touch firmware files

      Returns:
        List of TouchFile objects representing the touch firmware referenced
        by this model
      """
      touch = self.ChildNodeFromPath('/touch')
      files = {}
      for device in touch.subnodes.values():
        props = device.GetMergedProperties('touch-type')

        # Add a special property for the capitalised model name
        props['MODEL'] = self.name.upper()
        files[device.name] = TouchFile(
            self.GetTouchFilename(self._fdt_node.path, props, 'firmware-bin'),
            self.GetTouchFilename(self._fdt_node.path, props,
                                  'firmware-symlink'))
      return files

  class Property(object):
    """Represents a single property in a ChromeOS Configuration.

    Properties:
      name: The name of the property.
      value: The value of the property.
      type: The FDT type of the property (for now).
    """
    def __init__(self, fdt_prop):
      self._fdt_prop = fdt_prop
      self.name = fdt_prop.name
      self.value = fdt_prop.value
      # TODO(athilenius): Transform these int types to something more useful
      self.type = fdt_prop.type

    def GetPhandle(self):
      """Get the value of a property as a phandle

      Returns:
        Property's phandle as an integer (> 0)
      """
      return self._fdt_prop.GetPhandle()
